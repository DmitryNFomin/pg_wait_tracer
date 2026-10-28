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
 *   3. the Idle row and its named children on the SUMMARY path too, and
 *      their AAS -- a RATE over wall clock, which emit_idle_rows
 *      hardcoded to 0 until 2026-10-07, so this window rendered
 *      "Idle 19500 ms / AAS 0.00". Pinned here and not only in
 *      test_idle_accounting because this is the path a 900 s window
 *      takes: fixing the raw path alone would leave the demo unchanged.
 *      pct_db_time stays the not-applicable sentinel and is asserted so
 *      -- that one is correct (#435: the value was 0.0 before).
 *   4. the VERSION PREFLIGHT. Refusing v1/v2 in the reader is not enough:
 *      pgwt_visit_summaries SKIPS a file it cannot open, so a mixed window
 *      would come back as a plausible PARTIAL answer. The preflight must
 *      refuse, and refusing must mean "recompute from raw", not "empty".
 *  13. THE ROUTING DECISION ITSELF (#402, #403). Sections 1-5 all assume the
 *      request reached the summary path with files in front of it. The two
 *      bugs that produced a confident, specific, completely wrong screen were
 *      upstream of that: an archived file whose hour label was written with
 *      localtime_r on the capture box and re-derived with mktime() on the
 *      reader (displaced by the TZ delta, so every archive was excluded), and
 *      a preflight that APPROVED a window in which it had examined no file at
 *      all (so the response was built from zero records and labelled exact).
 *      13a refuses an unexamined window in the three shapes it occurs in,
 *      including nfiles == 2 with considered == 0, which is how it actually
 *      happened and which an `nfiles == 0` guard would approve; 13b pins TZ
 *      independence with the reader 5.5 h from the writer; 13c pins that a
 *      file the scanner can place NEITHER by header nor by name is refused
 *      rather than silently dropped from the overlap set.
 *   5. FALSE NEGATIVES. Section 1's agreement check is satisfied by 0 == 0,
 *      so every way the comparison can be starved gets its own case: no
 *      summary directory, a writer that wrote nothing, an unreadable file, a
 *      truncated header, a window with no overlap, and the case where the
 *      pacing event is absent rather than mis-accounted.
 *  15. v9 session measurements on disk: distinct fields round-trip, a short
 *      session is refused, v8 keeps its unsplit answer, and the observation
 *      counter saturates through the real writer and survives the format.
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

/* AAS is UNITLESS (ms / ms), so TOL_MS must never be reused for it: over this
 * file's 150 s window 0.01 of AAS is 1500 ms of misaccounted time, which is
 * 150x looser than the ms comparisons above and would accept a badly wrong
 * rate. Every AAS here is one double division of two exactly-representable
 * integer-nanosecond totals by the same denominator on both paths, so the
 * honest tolerance is float noise, not a budget. */
#define TOL_AAS 1e-9
#define NEAR_AAS(a, b) (fabs((double)(a) - (double)(b)) <= TOL_AAS)

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

/* Write the stream through the real writer. Return the accepted push count
 * so #681 can refuse a comparison if the writer could not commit its input. */
static int write_summaries(const char *dir, const struct pgwt_trace_event *ev,
                           int n)
{
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    if (!w) { printf("  FAIL: calloc writer\n"); tests_failed++; return -1; }
    if (pgwt_summary_writer_init(w, dir, 24, NULL) != 0) {
        printf("  FAIL: writer init on %s\n", dir);
        tests_failed++; free(w); return -1;
    }
    int accepted = 0;
    for (int i = 0; i < n; i++)
        if (pgwt_summary_push_event(w, &ev[i], 0, ev[i].old_event) == 0 &&
            w->enabled)
            accepted++;
    /* A zero push return also means "disabled"; enabled and the flush
     * counter distinguish acceptance from silently losing the stream. */
    /* Close the last, still-open second. */
    if (pgwt_summary_flush(w) != 0 || w->flush_failures_total != 0)
        accepted = -1;
    pgwt_summary_close(w);
    pgwt_summary_destroy(w);
    free(w);
    return accepted;
}

/* ── 12. A FAILED ROTATION FLUSH MUST DISCARD THE SECOND, NOT MISFILE IT ───
 *
 * accum_close() runs only on flush_accum's SUCCESS path, so a failed rotate
 * flush used to leave the accumulator ACTIVE -- and it was then written into
 * the NEXT hour's file at the following boundary, carrying a second_wall_ns
 * earlier than that file's own name-derived start. The reader's file-level
 * window filter places such a record wrongly, so one unwritable second became
 * a wrong answer in two windows rather than a short one in one. (The ERROR
 * text also claimed the second was "lost" when it was actually misfiled.)
 *
 * Reachable here because both the hour and the buffer size are fields:
 * forcing current_hour makes check_rotation rotate, and a short buffer makes
 * the flush inside it refuse. */
static void test_failed_rotation_discards_the_second(void)
{
    printf("--- 12. a failed rotation flush discards, never misfiles ---\n");
    const char *dir = fresh_dir("rotfail");
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    CHECK(w != NULL && pgwt_summary_writer_init(w, dir, 24, NULL) == 0,
          "writer opens");
    if (!w) return;

    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.new_event = EV_IO; e.cpu_ns = PGWT_CPU_NS_UNKNOWN; e.pid = 970;
    e.timestamp_ns = mono_origin() + 100 * MS;
    e.old_event = EV_LOCK; e.duration_ns = 10 * MS;
    pgwt_summary_push_event(w, &e, 0, e.old_event);
    CHECK(w->accum_active, "a second is open before the rotation");

    /* Force the rotation AND make its flush refuse. */
    w->current_hour = (w->current_hour + 1) % (366 * 24);
    size_t real = w->encode_buf_size;
    uint64_t recs_before = w->total_records_written;
    w->encode_buf_size = PGWT_SUMMARY_SERIALIZE_MAX - 1;
    pgwt_summary_check_rotation(w);
    w->encode_buf_size = real;

    CHECK(w->flush_failures_total >= 1,
          "the failed rotate flush is COUNTED (%llu)",
          (unsigned long long)w->flush_failures_total);
    CHECK(w->total_records_written == recs_before,
          "no record was written (%llu == %llu)",
          (unsigned long long)w->total_records_written,
          (unsigned long long)recs_before);
    CHECK(!w->accum_active,
          "and the open second is DISCARDED, not left active to be written "
          "into the next hour's file with an out-of-range wall_ns");

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
    uint32_t events_overflow;      /* v3 signal: events[] could not take an entry */
    uint64_t idle_ns_total;        /* v3 exact system-wide idle */
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
    c->events_overflow += rec->events_overflow;
    c->idle_ns_total += rec->idle_ns;
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
    /* ── THE IDLE ROW'S AAS, ON THE PATH THE 900 s DEMO WINDOW TAKES ─────
     *
     * AAS is a RATE (time / wall clock) and is defined for idle time; only a
     * SHARE OF DB TIME is not, which is why pct_db_time stays the
     * not-applicable sentinel (#435, was 0.0) and is
     * asserted so below. emit_idle_rows hardcoded aas = 0 until 2026-10-07,
     * so this window rendered "Idle 19500 ms / AAS 0.00" -- and the summary
     * path is the one should_use_summaries picks for a window this long, so
     * fixing only the raw path would have left the demo unchanged.
     *
     * Each value is pinned EXACTLY from the fixture's per-second ms times
     * SLICES, divided by the window's own wall clock -- never read back off a
     * row. "aas != 0" would accept ms (19500), ms/1e3 (19.5) or the DB Time
     * row's own rate (0.06); every one of those differs from 0.13 here. */
    struct { const char *name; int indent; double ms; } aas_rows[] = {
        { "Idle",                         0, PER_SEC_IDLE_MS * SLICES },
        { "Timeout:CheckpointWriteDelay", 2,  40.0 * SLICES },
        { "Client:ClientRead",            2,  25.0 * SLICES },
        { "Timeout:VacuumDelay",          2,  15.0 * SLICES },
        /* The hidden Activity share, 50 ms/s, as the labelled remainder. */
        { "Other idle (parked background loops)",           2,  50.0 * SLICES },
    };
    const int N_AAS_ROWS = (int)(sizeof(aas_rows) / sizeof(aas_rows[0]));

    /* NON-VACUITY: wall_ms comes from window_from_records above (150 s), not
     * from a clock read here. A 0 wall clock makes every assertion below
     * trivially true -- that shape is pinned separately at the end of this
     * section -- and a 1 ms wall clock would make aas == ms. */
    CHECK(wall_ms > 0 && !NEAR_MS(wall_ms, 1.0),
          "the window's wall clock is %.1f ms -- nonzero, and not 1 ms (where "
          "aas == ms would pass without dividing)", wall_ms);

    /* The raw path over the SAME window, so every row below is checked against
     * the fixture AND against the other implementation: a shared wrong formula
     * is the one thing agreement alone cannot see, which is why the expected
     * values are written out above rather than taken from `rawtm_u`. */
    struct pgwt_tm_result rawtm_u;
    pgwt_compute_time_model(ev, n, &f, 0, 0, wall_ms, &rawtm_u);

    double sum_aas = 0.0, sum_ms = 0.0;
    int aas_found = 0, aas_kids = 0;
    for (int i = 0; i < N_AAS_ROWS; i++) {
        double want = aas_rows[i].ms / wall_ms;
        const struct pgwt_tm_row *r = row_at(&tm, aas_rows[i].name,
                                             aas_rows[i].indent);
        CHECK(r != NULL, "summary idle row \"%s\" (indent %d) exists",
              aas_rows[i].name, aas_rows[i].indent);
        if (!r) continue;
        aas_found++;
        CHECK(NEAR_MS(r->time_ms, aas_rows[i].ms),
              "\"%s\" time=%.3f ms expected %.1f (the AAS below is only "
              "meaningful against the right ms)", aas_rows[i].name,
              r->time_ms, aas_rows[i].ms);
        CHECK(NEAR_AAS(r->aas, want),
              "\"%s\" summary aas=%.9f expected %.9f (%.1f ms / %.1f ms "
              "wall); 0.00 means the rate is still hardcoded",
              aas_rows[i].name, r->aas, want, aas_rows[i].ms, wall_ms);
        CHECK(r->pct_db_time == PGWT_PCT_DB_IDLE,
              "\"%s\" pct_db_time=%.4f must stay the not-applicable "
              "sentinel (-1): idle time genuinely has no share OF DB Time, "
              "and #435 made that a null on the wire, never a numeric 0",
              aas_rows[i].name, r->pct_db_time);
        /* Both paths, row by row: cross_validate compares them, and an AAS
         * that differed between them would be a drift the ms comparison above
         * cannot see. */
        const struct pgwt_tm_row *rr = row_at(&rawtm_u, aas_rows[i].name,
                                              aas_rows[i].indent);
        CHECK(rr != NULL && NEAR_AAS(rr->aas, r->aas),
              "\"%s\": raw aas %.9f vs summary aas %.9f", aas_rows[i].name,
              rr ? rr->aas : -1.0, r->aas);
        if (aas_rows[i].indent == 2) {
            aas_kids++;
            sum_aas += r->aas;
            sum_ms  += r->time_ms;
        }
    }
    CHECK(aas_found == N_AAS_ROWS && aas_kids == 4,
          "all %d idle rows found (got %d), 4 of them children (got %d) -- an "
          "empty or short breakdown would satisfy the sums below vacuously",
          N_AAS_ROWS, aas_found, aas_kids);

    /* THE PARENT'S OWN RATE IS NONZERO, asserted before the conservation sum:
     * "children sum to the parent" is satisfied by 0 == 0, which is precisely
     * how the broken code passed. 19500 / 150000 = 0.13. */
    const struct pgwt_tm_row *ip = row_at(&tm, "Idle", 0);
    CHECK(ip != NULL && ip->aas > 0.0 &&
          NEAR_AAS(ip->aas, PER_SEC_IDLE_MS * SLICES / wall_ms),
          "the Idle parent's rate is strictly positive and exact: %.9f "
          "expected %.9f", ip ? ip->aas : -1.0,
          PER_SEC_IDLE_MS * SLICES / wall_ms);
    CHECK(NEAR_MS(sum_ms, PER_SEC_IDLE_MS * SLICES),
          "the children's ms already sum to the parent (%.3f vs %.1f) -- the "
          "precondition for the AAS sum below", sum_ms,
          PER_SEC_IDLE_MS * SLICES);
    /* Compared against a constant derived from the fixture, NOT against the
     * parent row alone: two sides of one sum cannot fail. */
    CHECK(NEAR_AAS(sum_aas, PER_SEC_IDLE_MS * SLICES / wall_ms),
          "...so their AAS must follow: children sum %.9f expected %.9f "
          "(computed from the fixture, not from any row)", sum_aas,
          PER_SEC_IDLE_MS * SLICES / wall_ms);
    CHECK(ip != NULL && NEAR_AAS(sum_aas, ip->aas),
          "...and it equals the PARENT ROW's AAS (%.9f vs %.9f)", sum_aas,
          ip ? ip->aas : -1.0);
    /* DB Time's own rate is untouched: 9000 / 150000 = 0.06, not 0.19. */
    CHECK(NEAR_AAS(tm.rows[0].aas, PER_SEC_DB_MS * SLICES / wall_ms),
          "the DB Time row's AAS is unchanged at %.9f (got %.9f) -- idle time "
          "did not leak into it", PER_SEC_DB_MS * SLICES / wall_ms,
          tm.rows[0].aas);
    free(rawtm_u.rows);

    /* wall_ms == 0 on the summary path: the rate is undefined, the guard must
     * yield exactly 0 and never an inf/NaN. This is the one shape where an
     * idle AAS of 0 is right -- and the reason every assertion above uses the
     * window's real 150 s wall clock, since a 0 would satisfy them all
     * without computing anything. */
    struct pgwt_tm_result tmz;
    pgwt_compute_time_model_from_summaries(dir, from, to, &f, 0.0, &tmz);
    const struct pgwt_tm_row *zp = row_at(&tmz, "Idle", 0);
    CHECK(zp != NULL && NEAR_MS(zp->time_ms, PER_SEC_IDLE_MS * SLICES),
          "wall_ms=0: the Idle row still carries its %.1f ms (got %.3f), so "
          "this is \"no denominator\", not \"no idle time\"",
          PER_SEC_IDLE_MS * SLICES, zp ? zp->time_ms : -1.0);
    for (int i = 0; i < N_AAS_ROWS; i++) {
        const struct pgwt_tm_row *r = row_at(&tmz, aas_rows[i].name,
                                             aas_rows[i].indent);
        CHECK(r != NULL && r->aas == 0.0 && isfinite(r->aas),
              "wall_ms=0: \"%s\" aas must be exactly 0 and finite, got %.9f",
              aas_rows[i].name, r ? r->aas : -1.0);
    }
    free(tmz.rows);

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
          records == SLICES, "v%d records present (%d)",
          PGWT_SUMMARY_VERSION, records);

    int considered = -1, unusable = -1;
    CHECK(pgwt_summaries_window_current(dir, from, to, &considered,
                                        &unusable) == 1,
          "a freshly written window is current");
    CHECK(considered > 0,
          "...and it CONSIDERED %d file(s) — a gate that looked at nothing "
          "would also have returned 1", considered);
    CHECK(unusable == 0, "no unusable files (got %d)", unusable);

    /* THE IMMEDIATELY PRECEDING VERSION FIRST: v5 (the io_worker accounting
     * change, #315/#376, bumped v5 -> v6). A v5 second COUNTS io_worker time
     * and a v6 second does not, so a window holding both reports a DB Time
     * and an Idle that is neither. The byte layout is identical, which is
     * exactly why this case matters: nothing would fail to parse, so without
     * the header check the blend would be silent and plausible. */
    int t5 = set_version(dir, 5);
    CHECK(t5 > 0, "rewrote %d header(s) to version 5", t5);
    CHECK(pgwt_summaries_window_current(dir, from, to, &considered,
                                        &unusable) == 0,
          "a v5 window is REFUSED -- its io_worker accounting differs even "
          "though its bytes parse fine");
    CHECK(unusable == t5, "all %d v5 file(s) reported unusable (got %d)",
          t5, unusable);
    {
        struct rec_ctx c5;
        memset(&c5, 0, sizeof(c5));
        pgwt_visit_summaries(dir, 0, 0, rec_visitor, &c5);
        CHECK(c5.records == 0,
              "...and the reader returns 0 records for it, got %d", c5.records);
        struct pgwt_filter f5;
        memset(&f5, 0, sizeof(f5));
        struct pgwt_tm_result tm5;
        pgwt_compute_time_model_from_summaries(dir, from, to, &f5, 1000.0, &tm5);
        CHECK(tm5.db_time_ms == 0.0 && tm5.idle_time_ms == 0.0,
              "...so the summary answer over that window is 0.000/0.000 "
              "(got %.3f/%.3f) -- which is why the REFUSAL has to reach the "
              "server and become a RAW recompute, not an answer",
              tm5.db_time_ms, tm5.idle_time_ms);
        free(tm5.rows);
    }
    CHECK(set_version(dir, PGWT_SUMMARY_VERSION) == t5, "restore to v%d",
          PGWT_SUMMARY_VERSION);
    CHECK(pgwt_summaries_window_current(dir, from, to, &considered,
                                        &unusable) == 1,
          "the restored v%d window is current again", PGWT_SUMMARY_VERSION);

    /* THE DANGEROUS ONE: version 3.
     *
     * v3 was shipped by an earlier build of this branch with a DIFFERENT
     * layout -- no per-query idle_ns and no record trailer. Keeping the same
     * version number while adding those bytes would have made a v3 file
     * mis-parse under a v4 reader, and it fails in the worst shape: the
     * file-level preflight only inspects the header, so it would APPROVE the
     * file, and the visitor silently SKIPS blocks that fail to decode -- a
     * plausible partial window, no error, no raw fallback. Hence v4, and
     * hence this case. */
    int t3 = set_version(dir, 3);
    CHECK(t3 > 0, "rewrote %d header(s) to the OLD-layout version 3", t3);
    CHECK(pgwt_summaries_window_current(dir, from, to, &considered,
                                        &unusable) == 0,
          "an old-layout v3 window is REFUSED (so the caller recomputes from "
          "raw rather than decoding bytes that mean something else)");
    CHECK(unusable == t3, "all %d v3 file(s) reported unusable (got %d)",
          t3, unusable);
    {
        /* This only shows the REFUSAL works: the payload under that rewritten
         * header is still v4 bytes, so a zero-record read here proves the
         * header check fired, nothing about layout compatibility. The claim
         * "an unrefused old-layout block would mis-parse" needs a genuine
         * old-layout payload and is made in test_old_layout_is_incompatible()
         * below, against pgwt_summary_deserialize directly. */
        struct rec_ctx c3;
        memset(&c3, 0, sizeof(c3));
        pgwt_visit_summaries(dir, 0, 0, rec_visitor, &c3);
        CHECK(c3.records == 0,
              "...and the reader returns 0 records for it (the header check "
              "fired), got %d", c3.records);
    }
    CHECK(set_version(dir, PGWT_SUMMARY_VERSION) == t3, "restore to v%d",
          PGWT_SUMMARY_VERSION);
    CHECK(pgwt_summaries_window_current(dir, from, to, &considered,
                                        &unusable) == 1,
          "the restored v%d window is current again", PGWT_SUMMARY_VERSION);

    /* Now make it a v2 window: the pre-2026-10-06 accounting. The preflight
     * must refuse, because mixing v2 seconds with current seconds inside one
     * window blends two accounting rules and reports a DB Time that is
     * neither. */
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

    /* Back to the current version: the preflight must APPROVE again, so it is
     * reacting to the version and not permanently stuck. */
    CHECK(set_version(dir, PGWT_SUMMARY_VERSION) == touched,
          "restored headers to v%d", PGWT_SUMMARY_VERSION);
    CHECK(pgwt_summaries_window_current(dir, from, to, &considered,
                                        &unusable) == 1,
          "the restored v%d window is current again", PGWT_SUMMARY_VERSION);
    memset(&c, 0, sizeof(c));
    pgwt_visit_summaries(dir, 0, 0, rec_visitor, &c);
    CHECK(c.records == SLICES,
          "and the visitor reads all %d seconds again (got %d)", SLICES,
          c.records);
    free(ev);
}


/* Inspects one query's top_events[] so a test can state, rather than assume,
 * that the bounded list does not hold the event under test. */
struct topev_ctx {
    uint64_t query_id;
    int seen_records;
    int num_top_events;
    int idle_in_top;
};

static int topev_visitor(const struct pgwt_summary_accum *rec, void *vctx)
{
    struct topev_ctx *c = vctx;
    for (int q = 0; q < SUMMARY_MAX_QUERIES; q++) {
        const struct pgwt_summary_query *sq = &rec->queries[q];
        if (sq->query_id != c->query_id) continue;
        c->seen_records++;
        c->num_top_events = sq->num_top_events;
        for (int j = 0; j < sq->num_top_events; j++)
            if (pgwt_is_idle_event(sq->top_events[j].event_id))
                c->idle_in_top++;
    }
    return 0;
}

/* ── 6. THE BOUNDED TABLES ────────────────────────────────────────────────
 *
 * v3 moved idle time OUT of class_ns, which made the per-event tables the only
 * per-event source of it. Both tables are BOUNDED, so "read the idle total out
 * of the event list" -- which is what the first version of this branch did --
 * reintroduces, one level down, exactly the bug it replaced:
 *
 *   6a. queries[].top_events[] holds 8 entries. A query with 8 busier non-idle
 *       events pushes its pacing sleep out, and the query-filtered Idle reads
 *       0 beside a correct DB Time, so DB Time + Idle stops accounting for the
 *       window. (This is literally reason #3 given in src/compute.c for why
 *       the old summary_query_clientread_ns was wrong.)
 *   6b. events[] holds 1024. ~270 distinct PG wait events is headroom, not a
 *       proof -- Extension events are unbounded in principle -- and a full
 *       table under-reported Idle with no signal at all.
 *
 * Both totals now come from exact scalars accumulated at the writer; the
 * bounded lists only drive the NAMED breakdown, and whatever they miss shows
 * up as the labelled "Other idle (parked background loops)" row rather than vanishing.
 */
static void test_bounded_tables(void)
{
    printf("--- 6. bounded per-event tables must not bound the Idle TOTAL ---\n");

    /* 6a. Nine distinct events for one query, pacing LAST so it is the one
     * top_events[] drops. */
    const char *dir = fresh_dir("ninth");
    const uint64_t QN = 0x9999ULL;
    uint64_t origin = mono_origin();
    {
        struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
        CHECK(w != NULL && pgwt_summary_writer_init(w, dir, 24, NULL) == 0,
              "writer opens");
        for (int s2 = 0; s2 < SLICES && w; s2++) {
            uint64_t sec = origin + (uint64_t)s2 * ONE_SEC;
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            e.new_event = EV_IO; e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
            e.pid = 801; e.query_id = QN;
            /* 8 distinct NON-idle events, 1 ms each, claim every slot. */
            for (int k = 0; k < 8; k++) {
                e.timestamp_ns = sec + 100 * MS + (uint64_t)(k + 1) * MS;
                e.old_event = WEI(PG_WAIT_IPC, k);   /* 8 distinct IPC events */
                e.duration_ns = 1 * MS;
                pgwt_summary_push_event(w, &e, 0, e.old_event);
            }
            /* the 9th distinct event: a pacing sleep, 40 ms */
            e.timestamp_ns = sec + 100 * MS + 9 * MS;
            e.old_event = EV_CHECKPOINT_DELAY;
            e.duration_ns = 40 * MS;
            pgwt_summary_push_event(w, &e, 0, e.old_event);
        }
        if (w) { pgwt_summary_flush(w); pgwt_summary_close(w);
                 pgwt_summary_destroy(w); free(w); }
    }

    uint64_t from = 0, to = 0;
    int records = 0;
    CHECK(window_from_records(dir, &from, &to, &records) == 1 &&
          records == SLICES, "ninth-event fixture has %d records", records);
    double wall_ms = (double)(to - from) / 1e6;

    /* THE PRECONDITION that makes this test mean anything: the pacing event
     * really IS absent from top_events[]. If the list happened to hold it,
     * the Idle assertion below would pass for the wrong reason. */
    {
        struct topev_ctx tc;
        memset(&tc, 0, sizeof(tc));
        tc.query_id = QN;
        pgwt_visit_summaries(dir, 0, 0, topev_visitor, &tc);
        CHECK(tc.seen_records > 0, "inspected %d query records", tc.seen_records);
        CHECK(tc.num_top_events == SUMMARY_QUERY_TOP_EVENTS,
              "top_events is FULL (%d of %d)", tc.num_top_events,
              SUMMARY_QUERY_TOP_EVENTS);
        CHECK(tc.idle_in_top == 0,
              "...and holds NO idle event (%d) -- so the Idle total below "
              "cannot come from it", tc.idle_in_top);
    }

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    f.query_id = QN;
    struct pgwt_tm_result tm;
    pgwt_compute_time_model_from_summaries(dir, from, to, &f, wall_ms, &tm);
    CHECK(NEAR_MS(tm.db_time_ms, 8.0 * SLICES),
          "query DB Time = 8 x 1 ms/s = %.1f (got %.1f)", 8.0 * SLICES,
          tm.db_time_ms);
    CHECK(NEAR_MS(tm.idle_time_ms, 40.0 * SLICES),
          "query Idle = %.1f ms EXACTLY, even though the pacing event was "
          "pushed out of the 8-entry top_events list (got %.1f -- 0.0 is the "
          "bug this pins)", 40.0 * SLICES, tm.idle_time_ms);
    /* The breakdown cannot name it, so it must be LABELLED, not dropped. */
    const struct pgwt_tm_row *other = row_at(&tm, "Other idle (parked background loops)", 2);
    CHECK(other != NULL && NEAR_MS(other->time_ms, 40.0 * SLICES),
          "...and it appears as the labelled remainder row (got %.1f)",
          other ? other->time_ms : -1);
    free(tm.rows);

    /* 6b. The events[] overflow signal fires rather than silently truncating.
     * Push more distinct events than the table holds. */
    const char *odir = fresh_dir("overflow");
    {
        struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
        CHECK(w != NULL && pgwt_summary_writer_init(w, odir, 24, NULL) == 0,
              "overflow writer opens");
        uint64_t sec = mono_origin();
        struct pgwt_trace_event e;
        memset(&e, 0, sizeof(e));
        e.new_event = EV_IO; e.cpu_ns = PGWT_CPU_NS_UNKNOWN; e.pid = 802;
        /* SUMMARY_MAX_EVENTS + 200 distinct Extension ids, all in one second. */
        for (int k = 0; k < SUMMARY_MAX_EVENTS + 200 && w; k++) {
            e.timestamp_ns = sec + 100 * MS;
            e.old_event = WEI(PG_WAIT_EXTENSION, k);
            e.duration_ns = 1000;
            pgwt_summary_push_event(w, &e, 0, e.old_event);
        }
        /* plus a pacing sleep, which may or may not get a slot */
        if (w) {
            e.timestamp_ns = sec + 200 * MS;
            e.old_event = EV_CHECKPOINT_DELAY;
            e.duration_ns = 50 * MS;
            pgwt_summary_push_event(w, &e, 0, e.old_event);
            pgwt_summary_flush(w); pgwt_summary_close(w);
            pgwt_summary_destroy(w); free(w);
        }
    }
    {
        struct rec_ctx c;
        memset(&c, 0, sizeof(c));
        pgwt_visit_summaries(odir, 0, 0, rec_visitor, &c);
        CHECK(c.records >= 1, "overflow fixture produced a record");
        CHECK(c.events_overflow > 0,
              "the full events[] table is REPORTED (events_overflow=%u), not "
              "silently truncated", c.events_overflow);
        CHECK(c.idle_ns_total == 50ULL * MS,
              "...and the exact idle scalar still carries the whole 50 ms "
              "(got %llu ns) even though the table was full",
              (unsigned long long)c.idle_ns_total);
    }
}

/* ── 7. TOP QUERIES under a class filter must not charge idle as DB Time ──
 *
 * pgwt_compute_top_queries_from_summaries' class/event-filtered branch reads
 * queries[].top_events[], which deliberately RETAINS idle events so the Events
 * tab can show them -- so it is the one place v3's writer-side exclusion does
 * not reach, and it summed them straight into db_time_ns. The raw
 * pgwt_compute_top_queries skips idle records outright, so the same window
 * answered differently depending on which path served it. */
static void test_top_queries_filtered_excludes_idle(void)
{
    printf("--- 7. top_queries class filter excludes idle ---\n");
    const char *dir = fresh_dir("topq");
    struct pgwt_trace_event *ev = malloc(sizeof(*ev) * MAX_EV);
    int n = build_stream(ev, mono_origin());
    write_summaries(dir, ev, n);

    uint64_t from = 0, to = 0;
    int records = 0;
    CHECK(window_from_records(dir, &from, &to, &records) == 1, "records");
    double wall_ms = (double)(to - from) / 1e6;

    /* class=timeout. QID_B's only Timeout event is PgSleep (30 ms/s, DB Time).
     * The checkpointer's pacing sleeps carry no query id in this fixture, so
     * the discriminating case is Client: QID_A has ClientRead (idle, 25 ms/s)
     * and nothing else in the Client class, so a correct answer is 0 ms. */
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    snprintf(f.class_name, sizeof(f.class_name), "client");
    struct pgwt_queries_result rq, sq;
    pgwt_compute_top_queries(ev, n, &f, 0, 0, wall_ms, &rq);
    pgwt_compute_top_queries_from_summaries(dir, from, to, &f, wall_ms, &sq);

    double raw_a = 0, sum_a = 0;
    for (int i = 0; i < rq.num_rows; i++)
        if (rq.rows[i].query_id == QID_A) raw_a = rq.rows[i].total_ms;
    for (int i = 0; i < sq.num_rows; i++)
        if (sq.rows[i].query_id == QID_A) sum_a = sq.rows[i].total_ms;
    CHECK(NEAR_MS(raw_a, 0.0),
          "RAW: class=client, QID_A has only the idle ClientRead => 0 ms "
          "(got %.1f)", raw_a);
    CHECK(NEAR_MS(sum_a, 0.0),
          "SUMMARY: same => 0 ms, not the %.1f ms of ClientRead (got %.1f)",
          25.0 * SLICES, sum_a);
    CHECK(NEAR_MS(sq.db_time_ms, raw_a + 0.0) || sq.db_time_ms < TOL_MS,
          "SUMMARY db_time for the filtered view excludes idle (got %.3f)",
          sq.db_time_ms);
    free(rq.rows); free(sq.rows);

    /* NON-VACUITY: the same fixture under a class the query really does have
     * must come back NON-zero, or the zeros above prove nothing. */
    memset(&f, 0, sizeof(f));
    snprintf(f.class_name, sizeof(f.class_name), "lock");
    pgwt_compute_top_queries_from_summaries(dir, from, to, &f, wall_ms, &sq);
    double lock_a = 0;
    for (int i = 0; i < sq.num_rows; i++)
        if (sq.rows[i].query_id == QID_A) lock_a = sq.rows[i].total_ms;
    CHECK(NEAR_MS(lock_a, 20.0 * SLICES),
          "class=lock, QID_A = %.1f ms (got %.1f) -- so the zeros above are "
          "the idle exclusion, not an empty fixture", 20.0 * SLICES, lock_a);
    free(sq.rows);
    free(ev);
}


/* ── 8. parent == sum(children), EXACTLY, on every path ───────────────────
 *
 * Two reviews disagreed about whether this already held: one said the emitter
 * dropped a positive remainder of <= 1 us and suppressed negatives, the other
 * said equality held by construction on all three paths. Rather than argue,
 * this asserts it -- including a sub-microsecond hidden-Activity case, which
 * is precisely where the 1 us threshold would have shown.
 *
 * A NEGATIVE remainder (children exceeding the parent) should be impossible
 * on every path; it is surfaced as idle_children_excess_ms rather than
 * clamped, because it would mean the writer and the reader had diverged about
 * what counts as idle. Asserted zero here. */
static double sum_idle_children(const struct pgwt_tm_result *tm)
{
    /* indent 2 is shared with the per-CLASS sub-event rows, and a NAME test
     * cannot tell them apart: "Timeout:PgSleep" is a DB-Time sub-event of the
     * Timeout class while "Timeout:VacuumDelay" is an idle child. (The first
     * draft of this helper summed every "Timeout:*" row and reported children
     * exceeding the parent by exactly PgSleep's 4500 ms -- a bug in the test,
     * not in the product.)
     *
     * emit_idle_rows appends the Idle parent LAST, with its children
     * immediately after it, so the children are exactly the indent-2 rows
     * that follow the indent-0 "Idle" row. */
    double t = 0;
    int in_idle = 0;
    for (int i = 0; i < tm->num_rows; i++) {
        if (tm->rows[i].indent == 0 &&
            strcmp(tm->rows[i].name, "Idle") == 0) {
            in_idle = 1;
            continue;
        }
        if (!in_idle) continue;
        if (tm->rows[i].indent == 2) t += tm->rows[i].time_ms;
        else break;          /* left the Idle block */
    }
    return t;
}

static void test_idle_children_sum_exactly(void)
{
    printf("--- 8. Idle children sum to the parent exactly ---\n");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    /* 8a/8b: summary path, unfiltered and query-filtered. */
    const char *dir = fresh_dir("exact");
    struct pgwt_trace_event *ev = malloc(sizeof(*ev) * MAX_EV);
    int n = build_stream(ev, mono_origin());
    write_summaries(dir, ev, n);
    uint64_t from = 0, to = 0;
    int records = 0;
    CHECK(window_from_records(dir, &from, &to, &records) == 1, "records");
    double wall_ms = (double)(to - from) / 1e6;

    struct pgwt_tm_result tm;
    pgwt_compute_time_model_from_summaries(dir, from, to, &f, wall_ms, &tm);
    CHECK(NEAR_MS(sum_idle_children(&tm), tm.idle_time_ms),
          "summary unfiltered: children %.4f == parent %.4f",
          sum_idle_children(&tm), tm.idle_time_ms);
    CHECK(tm.idle_children_excess_ms == 0.0,
          "no negative remainder (excess %.6f)", tm.idle_children_excess_ms);
    free(tm.rows);

    memset(&f, 0, sizeof(f));
    f.query_id = QID_A;
    pgwt_compute_time_model_from_summaries(dir, from, to, &f, wall_ms, &tm);
    CHECK(NEAR_MS(sum_idle_children(&tm), tm.idle_time_ms),
          "summary query-filtered: children %.4f == parent %.4f",
          sum_idle_children(&tm), tm.idle_time_ms);
    CHECK(tm.idle_children_excess_ms == 0.0, "no negative remainder");
    free(tm.rows);

    memset(&f, 0, sizeof(f));
    snprintf(f.class_name, sizeof(f.class_name), "timeout");
    pgwt_compute_time_model_from_summaries(dir, from, to, &f, wall_ms, &tm);
    CHECK(NEAR_MS(sum_idle_children(&tm), tm.idle_time_ms),
          "summary class-filtered: children %.4f == parent %.4f",
          sum_idle_children(&tm), tm.idle_time_ms);
    free(tm.rows);
    free(ev);

    /* 8c: SUB-MICROSECOND hidden Activity on the raw path. 500 ns of Activity
     * is below the old 1 us threshold, so the remainder row was omitted and
     * the children did not sum to the parent. */
    {
        struct pgwt_trace_event tiny[2];
        memset(tiny, 0, sizeof(tiny));
        uint64_t t = 400ULL * 1000000000ULL;
        tiny[0].timestamp_ns = t + 1000000; tiny[0].pid = 901;
        tiny[0].old_event = EV_CLIENTREAD; tiny[0].new_event = EV_IO;
        tiny[0].duration_ns = 5 * MS; tiny[0].cpu_ns = PGWT_CPU_NS_UNKNOWN;
        tiny[1].timestamp_ns = t + 2000000; tiny[1].pid = 902;
        tiny[1].old_event = EV_ACTIVITY; tiny[1].new_event = EV_IO;
        tiny[1].duration_ns = 500;            /* 500 ns, hidden */
        tiny[1].cpu_ns = PGWT_CPU_NS_UNKNOWN;

        memset(&f, 0, sizeof(f));
        struct pgwt_tm_result rt;
        pgwt_compute_time_model(tiny, 2, &f, 0, 0, 1000.0, &rt);
        CHECK(rt.idle_time_ms > 0, "fixture has idle time (%.6f ms)",
              rt.idle_time_ms);
        const struct pgwt_tm_row *oth = row_at(&rt, "Other idle (parked background loops)", 2);
        CHECK(oth != NULL,
              "a 500 ns hidden-Activity remainder STILL gets its row (the old "
              "> 1 us threshold dropped it)");
        /* EXACT, not NEAR_MS: TOL_MS is 0.01 ms, which is 20x the 500 ns
         * remainder under test, so a tolerance-based comparison here could
         * not fail whether the row was emitted or not -- it looked like
         * coverage and was not. `oth != NULL` above is the load-bearing
         * assertion (it distinguishes `> 0` from `> 1e3`); this one now
         * actually constrains the arithmetic. */
        CHECK(fabs(sum_idle_children(&rt) - rt.idle_time_ms) < 1e-9,
              "raw sub-microsecond: children %.9f == parent %.9f EXACTLY",
              sum_idle_children(&rt), rt.idle_time_ms);
        CHECK(rt.idle_children_excess_ms == 0.0, "no negative remainder");
        free(rt.rows);
    }
}


/* ── 9. THE ROUND-2 LAYOUT IS GENUINELY INCOMPATIBLE WITH v4 ──────────────
 *
 * Why this exists: section 4 rewrites a v4 file's HEADER to 3 and shows the
 * reader refuses it. That proves the refusal fires -- but the bytes underneath
 * are still v4, so it says nothing about whether an old-layout block would
 * mis-parse, which is the entire justification for bumping the version. The
 * first version of this file claimed the one from the other, which is the same
 * mistake as the Timeout:* idle-children helper: a test measuring something
 * other than what it asserted.
 *
 * So this builds an ACTUAL round-2-layout payload -- current bytes with the
 * 8 bytes of per-query idle_ns, both record trailers, and the v9 session
 * additions removed -- and feeds it to pgwt_summary_deserialize as if it
 * were v4. No file, no compression, no block header: just the two layouts
 * and the parser.
 */
static size_t strip_v4_additions(const uint8_t *in, size_t in_size,
                                 const struct pgwt_summary_accum *src,
                                 uint8_t *out)
{
    /* Walk the current layout and remove fields absent before round 2.
     *   class_ns                      PGWT_NUM_CLASSES * 8
     *   events    (non-empty slots)   28 + HISTOGRAM_BUCKETS*8 each
     *   sessions  (non-empty slots)   44 each
     *   queries                       36 + NUM_CLASSES*8 + 1 + nte*20 + [8]
     *   trailer                       [12]
     * The bracketed parts are what round 2 added; v7's trailer and v9's
     * 12 session bytes must also come off to recover the historical layout. */
    const uint8_t *p = in;
    uint8_t *o = out;
    size_t n;

    n = PGWT_NUM_CLASSES * 8;        memcpy(o, p, n); p += n; o += n;

    for (int i = 0; i < SUMMARY_MAX_EVENTS; i++) {
        const struct pgwt_summary_event *e = &src->events[i];
        if (e->event_id == 0 && e->count == 0) continue;
        n = 28 + HISTOGRAM_BUCKETS * 8;  memcpy(o, p, n); p += n; o += n;
    }
    for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++) {
        const struct pgwt_summary_session *ss = &src->sessions[i];
        if (ss->pid == 0 && ss->db_time_ns == 0) continue;
        /* v9 session entry: 4+8+8+4+8 + v9's 8+4. */
        n = 44;                          memcpy(o, p, n - 12);
        p += n; o += n - 12;              /* DROP the v9 session fields */
    }
    for (int i = 0; i < SUMMARY_MAX_QUERIES; i++) {
        const struct pgwt_summary_query *q = &src->queries[i];
        if (q->query_id == 0 && q->count == 0) continue;
        n = 36 + PGWT_NUM_CLASSES * 8;   memcpy(o, p, n); p += n; o += n;
        uint8_t nte = *p;
        *o++ = nte; p++;
        n = (size_t)nte * 20;            memcpy(o, p, n); p += n; o += n;
        p += 8;                          /* DROP the v4 per-query idle_ns */
    }
    p += 12;                             /* DROP the v4 record trailer */
    /* v7 (#408) added a SECOND record trailer (the measured-CPU scalars),
     * also appended last. This helper's job is to reconstruct the layout as
     * it was BEFORE round 2, so every trailer the writer appends after that
     * point has to come off -- otherwise the size delta below counts v7's 40
     * bytes as part of round 2's addition and the assertion reads 68 where
     * it means 28. */
    p += 40;                             /* DROP the v7 record trailer */
    (void)in_size;
    return (size_t)(o - out);
}

static void test_old_layout_is_incompatible(void)
{
    printf("--- 9. a round-2-layout payload cannot be read as v4 ---\n");

    /* A record with two queries, each carrying idle time, so both the
     * per-query field and the trailer matter. */
    struct pgwt_summary_accum *acc = calloc(1, sizeof(*acc));
    CHECK(acc != NULL, "alloc accumulator");
    if (!acc) return;
    const char *dir = fresh_dir("layout");
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    CHECK(w != NULL && pgwt_summary_writer_init(w, dir, 24, NULL) == 0,
          "writer opens");
    if (w) {
        uint64_t sec = mono_origin();
        for (int i = 0; i < SLICE_N; i++) {
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            e.new_event = EV_IO; e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
            e.timestamp_ns = sec + 100 * MS + (uint64_t)(i + 1) * MS;
            e.pid = slice[i].pid; e.old_event = slice[i].ev;
            e.duration_ns = slice[i].ms * MS; e.query_id = slice[i].qid;
            pgwt_summary_push_event(w, &e, 0, e.old_event);
        }
        /* Snapshot the live accumulator before it is flushed away. */
        memcpy(acc, &w->accum, sizeof(*acc));
        pgwt_summary_flush(w); pgwt_summary_close(w);
        pgwt_summary_destroy(w); free(w);
    }
    CHECK(acc->idle_ns > 0, "the record has idle time (%llu ns)",
          (unsigned long long)acc->idle_ns);

    uint8_t *current = malloc(PGWT_SUMMARY_SERIALIZE_MAX);
    uint8_t *old = malloc(PGWT_SUMMARY_SERIALIZE_MAX);
    CHECK(current && old, "alloc buffers");
    if (!current || !old) { free(acc); free(current); free(old); return; }

    size_t current_size = pgwt_summary_serialize(acc, current,
                                                PGWT_SUMMARY_SERIALIZE_MAX);
    CHECK(current_size > 0, "current serialize produced %zu bytes", current_size);
    size_t old_size = strip_v4_additions(current, current_size, acc, old);

    /* Later additions must also come off to recover the actual old layout:
     * 12 per v9 session + 8 per query + 12 + v7's 40. */
    int nq = 0, ns = 0;
    for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++)
        if (!(acc->sessions[i].pid == 0 && acc->sessions[i].db_time_ns == 0)) ns++;
    for (int i = 0; i < SUMMARY_MAX_QUERIES; i++)
        if (!(acc->queries[i].query_id == 0 && acc->queries[i].count == 0)) nq++;
    CHECK(nq > 0, "the record has %d query entries", nq);
    CHECK(current_size - old_size == (size_t)ns * 12 + (size_t)nq * 8 + 12 + 40,
          "the round-2 payload is %zu bytes shorter = %d sessions x 12 + "
          "%d queries x 8 + 12 + 40", current_size - old_size, ns, nq);

    /* 9a. Current bytes round-trip exactly -- the control, without which a
     * mismatch below could just mean the parser is broken for everything. */
    {
        struct pgwt_summary_accum *back = calloc(1, sizeof(*back));
        CHECK(back != NULL, "alloc");
        if (back) {
            back->second_wall_ns = acc->second_wall_ns;
            back->num_events = acc->num_events;
            back->num_sessions = acc->num_sessions;
            back->num_queries = acc->num_queries;
            CHECK(pgwt_summary_deserialize(current, current_size, back,
                                           PGWT_SUMMARY_VERSION) == 0,
                  "current payload deserializes at the current version");
            CHECK(back->idle_ns == acc->idle_ns,
                  "...and the record idle_ns round-trips (%llu == %llu)",
                  (unsigned long long)back->idle_ns,
                  (unsigned long long)acc->idle_ns);
            free(back);
        }
    }

    /* 9b. the SAME record in round-2 layout, read as v4, must NOT come back
     * right. Either the parser refuses (short buffer) or it reads the wrong
     * numbers -- both are acceptable; silently reproducing the original is
     * not, because that is what "the bump was unnecessary" would look like. */
    {
        struct pgwt_summary_accum *back = calloc(1, sizeof(*back));
        CHECK(back != NULL, "alloc");
        if (back) {
            back->second_wall_ns = acc->second_wall_ns;
            back->num_events = acc->num_events;
            back->num_sessions = acc->num_sessions;
            back->num_queries = acc->num_queries;
            int rc = pgwt_summary_deserialize(old, old_size, back, 4);
            int wrong = (rc != 0) || (back->idle_ns != acc->idle_ns);
            CHECK(wrong,
                  "an old-layout payload read as v4 is REFUSED or mis-parsed "
                  "(rc=%d, idle_ns %llu vs the real %llu) -- this is why the "
                  "version had to move rather than the layout change in place",
                  rc, (unsigned long long)back->idle_ns,
                  (unsigned long long)acc->idle_ns);
            free(back);
        }
    }

    /* 9c. ...and read as v2 (its own field set) the old payload is fine, so
     * 9b is about the v4-only bytes and not about the payload being corrupt. */
    {
        struct pgwt_summary_accum *back = calloc(1, sizeof(*back));
        CHECK(back != NULL, "alloc");
        if (back) {
            back->second_wall_ns = acc->second_wall_ns;
            back->num_events = acc->num_events;
            back->num_sessions = acc->num_sessions;
            back->num_queries = acc->num_queries;
            CHECK(pgwt_summary_deserialize(old, old_size, back, 2) == 0,
                  "the same old-layout payload parses cleanly as v2 -- so 9b "
                  "is the v4-only fields, not a malformed buffer");
            free(back);
        }
    }

    free(acc); free(current); free(old);
}


/* ── 10. THE ZERO-PARENT CASE, which is the one the excess signal is FOR ───
 *
 * idle_children_excess_ms exists to detect the writer and the reader
 * disagreeing about which events are idle. The canonical shape of that
 * disagreement is a record written with one pacing mask and read with a wider
 * one: the writer's exact idle scalar is 0 (it saw no idle events) while the
 * reader's per-event pass names some as idle. Parent 0, children > 0.
 *
 * emit_idle_rows returned before comparing whenever the parent was <= 0, so
 * the single scenario the counter was added for was the one it could not see.
 * This reproduces it by writing with an EMPTY mask (what a PG14-16 trace gets)
 * and reading with PG18's (what a stale or wrong sidecar would cause), which
 * is the real-world path, not an injected value.
 */
static void test_zero_parent_excess_is_detected(void)
{
    printf("--- 10. parent 0 with non-zero children is reported ---\n");

    uint32_t saved = pgwt_idle_rule_timeout_mask();
    const char *dir = fresh_dir("zeroparent");

    /* WRITE with an empty mask: CheckpointWriteDelay is NOT idle to the
     * writer, so idle_ns stays 0 and the time lands in class_ns[TIMEOUT]. */
    pgwt_idle_rule_set_timeout_mask(0);
    {
        struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
        CHECK(w != NULL && pgwt_summary_writer_init(w, dir, 24, NULL) == 0,
              "writer opens");
        uint64_t origin = mono_origin();
        for (int s2 = 0; s2 < SLICES && w; s2++) {
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            e.new_event = EV_IO; e.cpu_ns = PGWT_CPU_NS_UNKNOWN; e.pid = 950;
            e.timestamp_ns = origin + (uint64_t)s2 * ONE_SEC + 100 * MS;
            e.old_event = EV_CHECKPOINT_DELAY; e.duration_ns = 40 * MS;
            pgwt_summary_push_event(w, &e, 0, e.old_event);
        }
        if (w) { pgwt_summary_flush(w); pgwt_summary_close(w);
                 pgwt_summary_destroy(w); free(w); }
    }

    uint64_t from = 0, to = 0;
    int records = 0;
    CHECK(window_from_records(dir, &from, &to, &records) == 1 &&
          records == SLICES, "written with the empty mask (%d records)",
          records);
    {
        struct rec_ctx c;
        memset(&c, 0, sizeof(c));
        pgwt_visit_summaries(dir, 0, 0, rec_visitor, &c);
        CHECK(c.idle_ns_total == 0,
              "the writer recorded NO idle time (idle_ns=%llu) -- the parent "
              "will be zero", (unsigned long long)c.idle_ns_total);
    }

    /* READ with PG18's mask: now the same event IS idle to the reader. */
    pgwt_idle_rule_set_timeout_mask(PGWT_IDLE_TIMEOUT_MASK_PG18);
    {
        struct pgwt_filter f;
        memset(&f, 0, sizeof(f));
        struct pgwt_tm_result tm;
        double wall_ms = (double)(to - from) / 1e6;
        pgwt_compute_time_model_from_summaries(dir, from, to, &f, wall_ms, &tm);
        CHECK(NEAR_MS(tm.idle_time_ms, 0.0),
              "parent Idle is 0.0 (from the writer's scalar), got %.3f",
              tm.idle_time_ms);
        CHECK(tm.idle_children_excess_ms > 0,
              "...and the mismatch IS REPORTED: "
              "idle_children_excess_ms = %.3f (0 would mean the one scenario "
              "this counter exists for is invisible)",
              tm.idle_children_excess_ms);
        CHECK(NEAR_MS(tm.idle_children_excess_ms, 40.0 * SLICES),
              "...with the right magnitude, %.1f ms (got %.1f)",
              40.0 * SLICES, tm.idle_children_excess_ms);
        free(tm.rows);
    }

    pgwt_idle_rule_set_timeout_mask(saved);
    CHECK(pgwt_idle_rule_timeout_mask() == saved, "mask restored");

    /* NON-VACUITY: a window written AND read with the same mask reports zero
     * excess, so the nonzero above is the mismatch and not a constant. */
    {
        const char *ok = fresh_dir("zeroparent_ok");
        struct pgwt_trace_event *ev = malloc(sizeof(*ev) * MAX_EV);
        int n = build_stream(ev, mono_origin());
        write_summaries(ok, ev, n);
        uint64_t f2 = 0, t2 = 0; int r2 = 0;
        CHECK(window_from_records(ok, &f2, &t2, &r2) == 1, "consistent fixture");
        struct pgwt_filter f;
        memset(&f, 0, sizeof(f));
        struct pgwt_tm_result tm;
        pgwt_compute_time_model_from_summaries(ok, f2, t2, &f,
                                               (double)(t2 - f2) / 1e6, &tm);
        CHECK(tm.idle_children_excess_ms == 0.0,
              "a consistently written/read window reports 0 excess (got %.6f)",
              tm.idle_children_excess_ms);
        free(tm.rows);
        free(ev);
    }
}

/* ── 11. A REFUSED SERIALIZE IS COUNTED, AND WRITES NOTHING ───────────────
 *
 * LZ4_compress_default() on 0 bytes returns 1, so before the explicit
 * encoded_size == 0 path a refused serialize wrote a block whose header still
 * claimed the record's event/session/query counts over an empty payload -- a
 * silently dropped second. And every flush_accum caller discarded the return
 * value, so flush_failures_total only ever moved on one internal path.
 *
 * A short buffer is the only way to reach the refusal, and it is reachable
 * here because the writer's buffer size is a field. */
static void test_refused_serialize_is_counted(void)
{
    printf("--- 11. a refused serialize is counted and writes no block ---\n");
    const char *dir = fresh_dir("shortbuf");
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    CHECK(w != NULL && pgwt_summary_writer_init(w, dir, 24, NULL) == 0,
          "writer opens");
    if (!w) return;

    uint64_t origin = mono_origin();
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.new_event = EV_IO; e.cpu_ns = PGWT_CPU_NS_UNKNOWN; e.pid = 960;
    e.timestamp_ns = origin + 100 * MS;
    e.old_event = EV_LOCK; e.duration_ns = 10 * MS;
    pgwt_summary_push_event(w, &e, 0, e.old_event);
    CHECK(w->accum_active, "a second is open");

    /* Mark this second as having dropped events, so the retry below also
     * exercises the overflow ROLL-UP and not just the failure counter. Set
     * directly rather than by pushing 1024 distinct events: the field is the
     * writer's own state and this test already reaches into it for
     * encode_buf_size. */
    w->accum.events_overflow = 3;
    uint64_t ovf_before = w->events_overflow_total;

    uint64_t recs_before = w->total_records_written;
    /* Make the serializer refuse: anything below PGWT_SUMMARY_SERIALIZE_MAX. */
    size_t real = w->encode_buf_size;
    w->encode_buf_size = PGWT_SUMMARY_SERIALIZE_MAX - 1;
    int rc = pgwt_summary_flush(w);
    w->encode_buf_size = real;

    CHECK(rc != 0, "the flush REPORTS failure (rc=%d)", rc);
    CHECK(w->flush_failures_total == 1,
          "...and it is COUNTED (flush_failures_total=%llu) -- the public "
          "wrappers used to return the status without touching the counter",
          (unsigned long long)w->flush_failures_total);
    CHECK(w->total_records_written == recs_before,
          "...and NO block reached the file (%llu == %llu): LZ4 on 0 bytes "
          "returns 1, so this used to write a header claiming the record's "
          "counts over an empty payload",
          (unsigned long long)w->total_records_written,
          (unsigned long long)recs_before);
    CHECK(w->accum_active,
          "...and the second stays OPEN so the next boundary retries it, "
          "rather than becoming a hole");

    /* CONTRAST: with the real buffer the same second flushes and is counted
     * as written -- so the assertions above are the refusal, not a writer
     * that never works. */
    int rc2 = pgwt_summary_flush(w);
    CHECK(rc2 == 0, "with the real buffer the flush succeeds (rc=%d)", rc2);
    CHECK(w->total_records_written == recs_before + 1,
          "...and the record IS written (%llu)",
          (unsigned long long)w->total_records_written);
    CHECK(w->flush_failures_total == 1, "the failure count does not grow");

    /* THE ROLL-UP IS NOT DOUBLE-COUNTED. The failed attempt left the
     * accumulator open, so the per-record events_overflow was still 3 when the
     * retry ran. Rolling it up at the TOP of flush_accum therefore added the
     * same 3 once per attempt: this second's three dropped events were reported
     * as six. Bounded and diagnostic-only, but a self-check that inflates under
     * failure reads as a second, independent problem -- the opposite of what a
     * counter is for. The roll-up belongs after every early return, with the
     * other success-side counters. */
    CHECK(w->events_overflow_total == ovf_before + 3,
          "the second's 3 dropped events are counted ONCE across a failed "
          "attempt and its retry (events_overflow_total=%llu, expected %llu)",
          (unsigned long long)w->events_overflow_total,
          (unsigned long long)(ovf_before + 3));

    pgwt_summary_close(w);
    pgwt_summary_destroy(w);
    free(w);
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

    /* 5b. AN EMPTY DIRECTORY. Genuinely different from 5a — nothing here is
     * unreadable, there is simply nothing at all — but the VERDICT is the
     * same, and that is #403. This case used to assert the opposite ("nothing
     * to blend, so approving is correct"), which is precisely how the bug
     * stayed invisible: approving kept the caller on the summary path, where
     * zero files produce 0.0 ms labelled "exact". "No summary file covers
     * this window" means the summary path CANNOT ANSWER, so it refuses and
     * the caller recomputes from raw. considered stays 0 so a refusal for
     * "I examined nothing" is still distinguishable from one for "I found
     * something broken". */
    const char *empty = fresh_dir("empty");
    considered = -1; unusable = -1;
    CHECK(pgwt_summaries_window_current(empty, 1, 2, &considered,
                                        &unusable) == 0,
          "an empty dir cannot answer the window: REFUSED (#403)");
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

    /* 5e. A WINDOW WITH NO OVERLAP. Healthy files on disk, but the requested
     * window is somewhere else entirely — the shape #402's timezone offset
     * manufactured out of a perfectly good archive. considered must be 0
     * (otherwise the gate would be claiming to have checked files it never
     * opened) and a verdict reached that way is a REFUSAL. */
    const char *ok = fresh_dir("overlap");
    n = build_stream(ev, mono_origin());
    write_summaries(ok, ev, n);
    CHECK(window_from_records(ok, &from, &to, &records) == 1, "overlap fixture");
    considered = -1; unusable = -1;
    /* Two hours before anything was written: the file's hour cannot overlap. */
    uint64_t far_to = (from > 7200ULL * ONE_SEC) ? from - 7200ULL * ONE_SEC : 1;
    CHECK(pgwt_summaries_window_current(ok, far_to - 1000, far_to,
                                        &considered, &unusable) == 0,
          "a window with no overlapping file is REFUSED (#403: it used to be "
          "approved, and the summary path then answered it with 0.0 ms)");
    CHECK(considered == 0,
          "...with considered=%d, which is WHY it refuses: a verdict reached "
          "without examining one file is not a verdict", considered);

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
            pgwt_summary_push_event(ww, &e, 0, e.old_event);
            e.timestamp_ns = sec + 2 * MS;
            e.old_event = EV_CLIENTREAD; e.duration_ns = 25 * MS;
            pgwt_summary_push_event(ww, &e, 0, e.old_event);
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

/* ══ 13. THE ROUTING GATE (#402, #403) ════════════════════════════════════
 * Two silent-wrong-number bugs lived on this path and NO gate could see
 * either one; both were found by a one-off measurement. They compose:
 *
 *   #402  an archived YYYY-MM-DD_HH.summary.lz4 file was named with
 *         localtime_r ON THE CAPTURE BOX and its start re-derived with
 *         mktime() ON THE READER, so every archived file was displaced by the
 *         TZ delta and summary_file_in_window() excluded it. A 1928 s replay
 *         on a CEST laptop answered 240.1 ms instead of 2,612,758.3 ms.
 *   #403  pgwt_summaries_window_current() approved a window in which it had
 *         examined NOTHING, so the caller stayed on the summary path and the
 *         response was built from zero records: 0.0 ms, "fidelity": "exact",
 *         on 14 of 14 consecutive 130 s windows whose raw value was ~170 s.
 *
 * The unit level cannot see the JSON label (server.c does not link into a
 * unit test), so what is pinned here is the routing decision plus the number
 * the summary path would otherwise have served: a zero over a window that
 * demonstrably holds 9000 ms. `considered` is asserted beside every verdict,
 * because "approved" and "approved without looking at a single file" are the
 * two outcomes this function spent four versions confusing. */

static int read_summary_hdr(const char *path,
                            struct pgwt_trace_file_header *hdr)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    int ok = (fread(hdr, sizeof(*hdr), 1, fp) == 1 &&
              hdr->magic == PGWT_SUMMARY_MAGIC);
    fclose(fp);
    return ok;
}

/* Archive dir/current.summary exactly as the writer's rotation does -- a
 * plain rename() -- but with the hour label formatted in a timezone WE pick
 * (gmtime_r here, i.e. a UTC capture box) rather than the reader's. That is
 * the only difference between this fixture and production, and it is the
 * whole of #402. Returns 1 and fills `out` with the new basename. */
static int archive_current_as_utc(const char *dir, uint64_t start_ns,
                                  char *out, size_t outsz)
{
    time_t sec = (time_t)(start_ns / ONE_SEC);
    struct tm g;
    if (!gmtime_r(&sec, &g)) return 0;
    char name[64];
    snprintf(name, sizeof(name), "%04d-%02d-%02d_%02d.summary.lz4",
             g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour);
    char from[700], to[700];
    snprintf(from, sizeof(from), "%s/current.summary", dir);
    snprintf(to, sizeof(to), "%s/%s", dir, name);
    if (rename(from, to) != 0) return 0;
    snprintf(out, outsz, "%s", name);
    return 1;
}

/* The start pgwt_scan_summary_files USED to derive: the name read in the
 * READER's current timezone. Kept here so the fixture can prove it really is
 * displaced -- without this the TZ case could pass with a 0 h offset and
 * assert nothing at all. */
static uint64_t name_start_ns_in_local_tz(const char *basename)
{
    int y = 0, mo = 0, d = 0, h = 0;
    if (sscanf(basename, "%4d-%2d-%2d_%2d.summary.lz4", &y, &mo, &d, &h) != 4)
        return 0;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == (time_t)-1) return 0;
    return (uint64_t)t * ONE_SEC;
}

static int count_summary_files(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if ((len >= 8 && strcmp(e->d_name + len - 8, ".summary") == 0) ||
            (len >= 12 && strcmp(e->d_name + len - 12, ".summary.lz4") == 0))
            n++;
    }
    closedir(d);
    return n;
}

static int unlink_summary_files(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if ((len >= 8 && strcmp(e->d_name + len - 8, ".summary") == 0) ||
            (len >= 12 && strcmp(e->d_name + len - 12, ".summary.lz4") == 0)) {
            char p[700];
            snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
            if (unlink(p) == 0) n++;
        }
    }
    closedir(d);
    return n;
}

static double summary_db_time(const char *dir, uint64_t from, uint64_t to)
{
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;
    double wall_ms = (to > from) ? (double)(to - from) / 1e6 : 1000.0;
    pgwt_compute_time_model_from_summaries(dir, from, to, &f, wall_ms, &tm);
    double v = tm.db_time_ms;
    free(tm.rows);
    return v;
}

/* 13a. #403: a window the summary path examined NO file for must be refused,
 * in all three shapes it occurs in -- and the obvious `nfiles == 0` fix
 * covers only one of them. */
static void test_routing_refuses_an_unexamined_window(void)
{
    printf("--- 13a. a window with no examined file is REFUSED (#403) ---\n");

    /* fresh_dir() hands back a STATIC buffer, and this case keeps THREE
     * fixtures alive at once -- copy each path or they all alias the last
     * one, which silently turns every assertion below into a statement about
     * a different directory. */
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", fresh_dir("routing_unexamined"));
    struct pgwt_trace_event *ev = malloc(sizeof(*ev) * MAX_EV);
    if (!ev) { printf("  FAIL: malloc\n"); tests_failed++; return; }
    int n = build_stream(ev, mono_origin());
    write_summaries(dir, ev, n);

    uint64_t from = 0, to = 0;
    int records = 0;
    int considered = -1, unusable = -1;

    /* NON-VACUITY FIRST. Everything below compares against this dir, so if
     * the writer had failed the refusals would all be trivially true. */
    CHECK(window_from_records(dir, &from, &to, &records) == 1 &&
          records == SLICES,
          "fixture holds %d seconds of records (expected %d)", records,
          SLICES);
    CHECK(pgwt_summaries_window_current(dir, from, to, &considered,
                                        &unusable) == 1 && considered > 0 &&
          unusable == 0,
          "control: the records' own window is APPROVED after examining %d "
          "file(s), %d unusable", considered, unusable);
    CHECK(NEAR_MS(summary_db_time(dir, from, to), PER_SEC_DB_MS * SLICES),
          "control: the summary path answers %.1f ms over it (expected %.1f) "
          "-- so a 0.0 below is a WRONG number, not an empty capture",
          summary_db_time(dir, from, to), PER_SEC_DB_MS * SLICES);

    /* 13a-1. THE SHAPE THAT WAS MEASURED: files on disk, none of them
     * resolving into the requested window. This is the case #402 produced
     * (a TZ-displaced archive) and the one `nfiles == 0` does NOT cover. */
    /* 48 h, not 2 h: a reader TZ offset of exactly N hours displaces an
     * archive's name-derived start by N hours, so a window "2 h before the
     * data" lands ON the displaced file on a UTC+2 host and the case stops
     * being about an unexamined window at all. 48 h clears every real UTC
     * offset (max 14 h). */
    uint64_t far_to = (from > 48ULL * 3600 * ONE_SEC)
                        ? from - 48ULL * 3600 * ONE_SEC : 1;
    uint64_t far_from = far_to - 1000;
    considered = -1; unusable = -1;
    CHECK(pgwt_summaries_window_current(dir, far_from, far_to, &considered,
                                        &unusable) == 0,
          "a window no file resolves into is REFUSED, so the caller "
          "recomputes from RAW");
    CHECK(considered == 0,
          "...and it says so: considered=%d (the refusal is 'I examined "
          "nothing', not 'I found something broken')", considered);
    CHECK(summary_db_time(dir, far_from, far_to) == 0.0,
          "...and the answer it would otherwise have served is 0.0 ms "
          "(got %.3f) over a directory holding %.1f ms -- the silent wrong "
          "number, which only the refusal above prevents",
          summary_db_time(dir, far_from, far_to), PER_SEC_DB_MS * SLICES);

    /* 13a-2. THE WRONG FIX, MADE UNREACHABLE. `nfiles == 0 -> return 0` is
     * the obvious guard and it would approve this: TWO files are listed and
     * NEITHER is examined, which is exactly what #402 + #403 did in
     * production (nfiles == 2, considered == 0, answer 240.1 ms of a
     * 2,612,758.3 ms window, labelled exact). The guard has to key on
     * `considered`, so this case exists to keep anyone from "simplifying" it
     * back to a file count. */
    char two[600];
    snprintf(two, sizeof(two), "%s", fresh_dir("routing_two_files"));
    write_summaries(two, ev, n);
    struct pgwt_trace_file_header hdr;
    char cur[700], arch[64];
    snprintf(cur, sizeof(cur), "%s/current.summary", two);
    CHECK(read_summary_hdr(cur, &hdr) == 1, "two-file fixture: header read");
    CHECK(archive_current_as_utc(two, hdr.start_time_ns, arch,
                                 sizeof(arch)) == 1,
          "two-file fixture: archived as %s", arch);
    write_summaries(two, ev, n);           /* a second, current file */
    struct pgwt_summary_file_entry ents[8];
    int nfiles = pgwt_scan_summary_files(two, ents, 8);
    CHECK(count_summary_files(two) == 2 && nfiles == 2,
          "two-file fixture: %d file(s) on disk, scanner lists %d -- so an "
          "`nfiles == 0` guard could not fire here",
          count_summary_files(two), nfiles);
    considered = -1; unusable = -1;
    CHECK(pgwt_summaries_window_current(two, far_from, far_to, &considered,
                                        &unusable) == 0,
          "nfiles=2 with considered=0 is still REFUSED");
    CHECK(considered == 0, "...considered=%d", considered);

    /* 13a-3. NO FILE AT ALL, with a real window. #403's own reproduction was
     * "delete both summary files from disk and the answer is still
     * db_time_ms=0, path=summary, exact". */
    CHECK(unlink_summary_files(dir) == 1,
          "deleted the fixture's summary file(s)");
    considered = -1; unusable = -1;
    CHECK(pgwt_summaries_window_current(dir, from, to, &considered,
                                        &unusable) == 0,
          "an EMPTIED trace dir is REFUSED over the very window it used to "
          "answer (the shape #403 was reported with)");
    CHECK(considered == 0, "...considered=%d", considered);

    /* 13a-4. WHAT THIS GUARD STILL CANNOT SEE, asserted so that the comment
     * in src/summary_reader.c is a decision and not a hope: a GAP BETWEEN two
     * in-window files. Both files are examined, both are the current version,
     * so the window is approved -- and the hour between them contributes
     * nothing, silently. Only a record- or coverage-level check can catch
     * that; `considered` cannot. */
    char gap[600];
    snprintf(gap, sizeof(gap), "%s", fresh_dir("routing_gap"));
    write_summaries(gap, ev, n);
    snprintf(cur, sizeof(cur), "%s/current.summary", gap);
    CHECK(read_summary_hdr(cur, &hdr) == 1, "gap fixture: header read");
    uint64_t old_start = hdr.start_time_ns - 7200ULL * ONE_SEC;
    {
        FILE *fp = fopen(cur, "r+b");
        struct pgwt_trace_file_header h2 = hdr;
        h2.start_time_ns = old_start;
        CHECK(fp != NULL && fwrite(&h2, sizeof(h2), 1, fp) == 1,
              "gap fixture: backdated one file's header by 2 h");
        if (fp) fclose(fp);
    }
    CHECK(archive_current_as_utc(gap, old_start, arch, sizeof(arch)) == 1,
          "gap fixture: archived the backdated file as %s", arch);
    write_summaries(gap, ev, n);           /* the current hour */
    /* The window is derived from the two headers, not from the other
     * fixture's records: it must hold both files whatever order the cases in
     * this file run in and however long they take. */
    uint64_t gap_to = hdr.start_time_ns + 3600ULL * ONE_SEC;
    considered = -1; unusable = -1;
    CHECK(pgwt_summaries_window_current(gap, old_start, gap_to, &considered,
                                        &unusable) == 1 && considered == 2,
          "KNOWN BLIND SPOT: two in-window files with a missing hour BETWEEN "
          "them is approved (considered=%d) -- this guard proves it looked at "
          "something, never that the window is covered", considered);

    free(ev);
}

/* 13b. #402: an archived file must be found in its window on a reader whose
 * timezone is not the capture box's. */
static void test_archived_file_is_timezone_independent(void)
{
    printf("--- 13b. an archived file is found in any reader TZ (#402) ---\n");

    const char *dir = fresh_dir("routing_tz");
    struct pgwt_trace_event *ev = malloc(sizeof(*ev) * MAX_EV);
    if (!ev) { printf("  FAIL: malloc\n"); tests_failed++; return; }
    int n = build_stream(ev, mono_origin());
    write_summaries(dir, ev, n);

    /* The window comes from the records while the file is still
     * current.summary -- that path reads the HEADER and was never affected by
     * #402, so the fixture can state its own window even on the broken tree.
     * Deriving it after the rename would have been impossible: the broken
     * scanner excludes the archive, the visitor returns 0 records, and the
     * test would have had no window to assert about. */
    uint64_t from = 0, to = 0;
    int records = 0;
    CHECK(window_from_records(dir, &from, &to, &records) == 1 &&
          records == SLICES, "TZ fixture holds %d seconds (expected %d)",
          records, SLICES);

    struct pgwt_trace_file_header hdr;
    char cur[700], arch[64];
    snprintf(cur, sizeof(cur), "%s/current.summary", dir);
    CHECK(read_summary_hdr(cur, &hdr) == 1 && hdr.start_time_ns > 0,
          "the uncompressed header carries start_time_ns -- the fix's whole "
          "premise, and an archived file is a plain rename() of this file");
    CHECK(archive_current_as_utc(dir, hdr.start_time_ns, arch,
                                 sizeof(arch)) == 1,
          "archived with a UTC hour label (as the capture box does): %s",
          arch);
    CHECK(count_summary_files(dir) == 1,
          "exactly one summary file, and it is the ARCHIVED one -- without "
          "this, a surviving current.summary would answer the window and the "
          "assertions below would say nothing about archives");

    /* The reader is now in a timezone 5.5 h from the writer's. */
    char *saved_tz = getenv("TZ");
    char saved_copy[128] = "";
    int had_tz = (saved_tz != NULL);
    if (had_tz) snprintf(saved_copy, sizeof(saved_copy), "%s", saved_tz);
    setenv("TZ", "Asia/Kolkata", 1);
    tzset();

    /* NON-VACUITY OF THE TZ ITSELF. If the offset were 0 (reader == writer,
     * which is the configuration under which three clean rehearsals missed
     * this bug) the old code would have passed too. Assert the precise
     * condition that made summary_file_in_window() exclude the file: the
     * NAME-derived hour, read in this timezone, ends before the window
     * starts. If this fails the fixture cannot detect #402 and must say so
     * rather than report a pass. */
    uint64_t name_start = name_start_ns_in_local_tz(arch);
    CHECK(name_start > 0, "the archived name parses as an hour (%s)", arch);
    CHECK(name_start + 3600ULL * ONE_SEC < from,
          "under TZ=Asia/Kolkata the NAME-derived window ends %.1f s before "
          "the records begin -- which is why the old scanner excluded this "
          "file; a 0 h offset here would make this test vacuous",
          (double)((int64_t)from - (int64_t)(name_start + 3600ULL * ONE_SEC))
              / 1e9);

    struct pgwt_summary_file_entry ents[8];
    int nfiles = pgwt_scan_summary_files(dir, ents, 8);
    CHECK(nfiles == 1, "the scanner lists the archived file (%d)", nfiles);
    CHECK(nfiles == 1 && ents[0].start_wall_ns == hdr.start_time_ns,
          "...with start_wall_ns taken from its HEADER: scanner says %llu, "
          "header says %llu, the name read in this TZ says %llu",
          nfiles == 1 ? (unsigned long long)ents[0].start_wall_ns : 0ULL,
          (unsigned long long)hdr.start_time_ns,
          (unsigned long long)name_start);

    int considered = -1, unusable = -1;
    CHECK(pgwt_summaries_window_current(dir, from, to, &considered,
                                        &unusable) == 1 && considered == 1 &&
          unusable == 0,
          "the archived file is IN WINDOW on a reader 5.5 h from the writer "
          "(considered=%d, unusable=%d)", considered, unusable);

    struct rec_ctx c;
    memset(&c, 0, sizeof(c));
    pgwt_visit_summaries(dir, from, to, rec_visitor, &c);
    CHECK(c.records == SLICES,
          "and the visitor reads all %d seconds out of it (got %d) -- the "
          "cross-TZ replay that answered 240.1 ms read 0 here", SLICES,
          c.records);
    CHECK(NEAR_MS(summary_db_time(dir, from, to), PER_SEC_DB_MS * SLICES),
          "DB Time over the archived window is %.1f ms, expected %.1f",
          summary_db_time(dir, from, to), PER_SEC_DB_MS * SLICES);

    /* THE FALLBACK MUST STILL LIST A FILE IT CANNOT READ. The preflight's
     * ability to refuse depends on a rotated file being LISTED even when its
     * contents are unusable; deriving the start from the header must not turn
     * a bad header into an INVISIBLE file, which would be a silent partial
     * window instead of a refusal. Checked with TZ=UTC, matching the name, so
     * the file is genuinely in window and the verdict is about the header. */
    setenv("TZ", "UTC", 1);
    tzset();
    {
        char path[800];
        snprintf(path, sizeof(path), "%s/%s", dir, arch);
        CHECK(truncate(path, 8) == 0, "truncated %s to 8 bytes", arch);
        nfiles = pgwt_scan_summary_files(dir, ents, 8);
        CHECK(nfiles == 1,
              "a header-less archive is STILL LISTED via its name (%d) -- "
              "dropping it would hide it from the preflight", nfiles);
        considered = -1; unusable = -1;
        CHECK(pgwt_summaries_window_current(dir, from, to, &considered,
                                            &unusable) == 0,
              "...and the window is REFUSED");
        CHECK(considered == 1 && unusable == 1,
              "...as UNUSABLE (considered=%d unusable=%d), not as absent",
              considered, unusable);
    }

    if (had_tz) setenv("TZ", saved_copy, 1);
    else unsetenv("TZ");
    tzset();
    free(ev);
}

/* 13c. A FILE THE SCANNER CANNOT PLACE AT ALL. The start now comes from the
 * header with the name as a fallback, so a file whose header is unreadable AND
 * whose name mktime() cannot place has NO derivable start. The dangerous
 * outcome is not "refused" but "silently excluded from every window": a
 * garbage start lands the file in the year 0 or the year 9999, the overlap
 * test drops it, and neither the preflight nor the visitor ever sees it --
 * which is a silent partial window, the exact shape of #402. Unknown coverage
 * must count against every window. */
static void test_unplaceable_archive_is_refused(void)
{
    printf("--- 13c. an archive with no derivable start is REFUSED ---\n");

    char dir[600];
    snprintf(dir, sizeof(dir), "%s", fresh_dir("routing_unplaceable"));
    struct pgwt_trace_event *ev = malloc(sizeof(*ev) * MAX_EV);
    if (!ev) { printf("  FAIL: malloc\n"); tests_failed++; return; }
    int n = build_stream(ev, mono_origin());
    write_summaries(dir, ev, n);

    uint64_t from = 0, to = 0;
    int records = 0;
    CHECK(window_from_records(dir, &from, &to, &records) == 1 &&
          records == SLICES, "fixture holds %d seconds (expected %d)",
          records, SLICES);
    struct pgwt_trace_file_header hdr;
    char cur[700], arch[64];
    snprintf(cur, sizeof(cur), "%s/current.summary", dir);
    CHECK(read_summary_hdr(cur, &hdr) == 1, "header read");
    CHECK(archive_current_as_utc(dir, hdr.start_time_ns, arch,
                                 sizeof(arch)) == 1, "archived as %s", arch);

    /* A matching NAME the clock cannot place, with no readable header. */
    char junk[700];
    snprintf(junk, sizeof(junk), "%s/0000-00-00_00.summary.lz4", dir);
    {
        FILE *fp = fopen(junk, "wb");
        CHECK(fp != NULL && fwrite("12345678", 1, 8, fp) == 8,
              "wrote an unplaceable, header-less archive");
        if (fp) fclose(fp);
    }

    struct pgwt_summary_file_entry ents[8];
    int nfiles = pgwt_scan_summary_files(dir, ents, 8);
    CHECK(nfiles == 2,
          "the scanner lists BOTH files (%d) -- a file it cannot place must "
          "not simply vanish from the listing", nfiles);
    int considered = -1, unusable = -1;
    CHECK(pgwt_summaries_window_current(dir, from, to, &considered,
                                        &unusable) == 0,
          "the window is REFUSED while that file is present");
    CHECK(considered == 2 && unusable == 1,
          "...because the unplaceable file was EXAMINED and found unusable "
          "(considered=%d unusable=%d), not quietly dropped from the overlap "
          "set", considered, unusable);

    /* CONTROL: remove it and the same window is approved again, so the
     * refusal above is about that file and not a function stuck at 0. */
    CHECK(unlink(junk) == 0, "removed the unplaceable file");
    considered = -1; unusable = -1;
    CHECK(pgwt_summaries_window_current(dir, from, to, &considered,
                                        &unusable) == 1 && considered == 1 &&
          unusable == 0,
          "the archived file alone is APPROVED again (considered=%d "
          "unusable=%d)", considered, unusable);
    CHECK(NEAR_MS(summary_db_time(dir, from, to), PER_SEC_DB_MS * SLICES),
          "...and answers %.1f ms, expected %.1f",
          summary_db_time(dir, from, to), PER_SEC_DB_MS * SLICES);
    free(ev);
}

/* ══ 14. THE BOUNDED sessions[] TABLE MUST REPORT WHAT IT DROPS (#555) ═════
 *
 * sessions[] holds SUMMARY_MAX_SESSIONS (1024) pids per second. Past that
 * find_or_insert_session returns NULL and the pid's time was DROPPED -- from
 * accum_event, from the open-interval credit, and on the read side from
 * pgwt_summary_deserialize -- while class_ns and idle_ns, which never consult
 * that table, kept every nanosecond. So Top Sessions summed to LESS than
 * Overview's DB Time with nothing anywhere saying why.
 *
 * WHY THAT IS WORSE THAN A BOUND. On the RAW path the two totals agree to the
 * last bit: Overview time_model DB Time 2,495,784.951108 ms against
 * Sigma top_sessions.db_time_ms 2,495,784.951108 ms over an 1851 s / 4.3M-event
 * PG18 trace. A reader who sees them disagree on the SUMMARY path therefore
 * has exactly two candidate explanations -- "this capture exceeded 1024
 * concurrent pids" and "the accounting is broken" -- and before this counter
 * nothing distinguished them. events_overflow had had that signal since v3.
 *
 * WHAT MAKES THESE ASSERTIONS EVIDENCE RATHER THAN DECORATION. `overflow > 0`
 * on an overflowing fixture would also pass for a counter that ticks once per
 * EVENT, or once per flush, or always. So:
 *   - the count is asserted EXACTLY (`== EXTRA`), per denied lookup, with the
 *     pushes-per-pid varied so the unit itself is pinned;
 *   - it is asserted == 0 at 4 pids AND at exactly 1024 -- the same assertion
 *     that goes red in 14b, so a free-running counter fails;
 *   - the lost nanoseconds are measured from the OTHER SIDE: class_ns is
 *     accumulated by code that never looks at sessions[], so
 *     class_ns - Sigma sessions[].db_time_ns is an independent witness and
 *     the counter must account for the whole of it;
 *   - every probe reports whether the fixture RAN (a second opened, class_ns
 *     positive, a record written), because `0 == 0` on a fixture that never
 *     produced a second is how a gate approves while blind -- 14f pins the
 *     empty shape so it cannot be mistaken for a tested zero;
 *   - 14g pins the counter's MEANING in both directions: idle-only denials
 *     are 0 while the table is demonstrably full and the idle time
 *     demonstrably arrived, and a MIXED second counts its non-idle denials
 *     and only those -- an assertion a guard that simply stopped counting
 *     cannot pass;
 *   - 14e asserts the reader's WARN, not only its counter, because that
 *     field reaches nobody on the read side.
 *
 * TIMING. One second, every timestamp a literal offset from the writer's
 * origin, every duration a literal; no assertion depends on wall time.
 */

#define SESS_ONE_NS (1ULL * MS)   /* one interval per push, 1 ms each */

struct sess_probe {
    int      opened;        /* a second was open when the pushes ended      */
    int      placed;        /* acc->num_sessions                            */
    uint32_t overflow;      /* acc->sessions_overflow BEFORE the flush      */
    uint64_t sessions_ns;   /* Sigma sessions[].db_time_ns                  */
    uint64_t class_ns;      /* Sigma class_ns -- never reads sessions[]     */
    uint64_t idle_ns;       /* acc->idle_ns -- the idle fixtures' witness   */
    uint64_t total_after;   /* w->sessions_overflow_total AFTER the flush   */
    uint32_t accum_after;   /* acc->sessions_overflow AFTER the flush       */
    uint64_t records;       /* w->total_records_written                     */
};

/* `npids` distinct pids, `pushes` 1 ms intervals of `ev` each, all in ONE
 * second, through the REAL writer. `ev` is a parameter because the counter's
 * meaning turns on whether the event is idle: see 14g. */
static void sess_probe_run(const char *name, int npids, int pushes,
                           uint32_t ev, struct sess_probe *out)
{
    memset(out, 0, sizeof(*out));
    const char *dir = fresh_dir(name);
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    if (!w) return;
    if (pgwt_summary_writer_init(w, dir, 24, NULL) != 0) { free(w); return; }

    uint64_t sec = mono_origin();
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.new_event = EV_IO;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    e.timestamp_ns = sec + 100 * MS;
    e.old_event = ev;
    e.duration_ns = SESS_ONE_NS;
    e.query_id = 0;
    for (int k = 0; k < npids; k++) {
        e.pid = (uint32_t)(5000 + k); /* distinct and never 0 */
        for (int j = 0; j < pushes; j++)
            pgwt_summary_push_event(w, &e, 0, e.old_event);
    }

    out->opened   = w->accum_active ? 1 : 0;
    out->placed   = w->accum.num_sessions;
    out->overflow = w->accum.sessions_overflow;
    for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++)
        out->sessions_ns += w->accum.sessions[i].db_time_ns;
    for (int i = 0; i < PGWT_NUM_CLASSES; i++)
        out->class_ns += w->accum.class_ns[i];
    out->idle_ns = w->accum.idle_ns;

    pgwt_summary_flush(w);
    out->total_after = w->sessions_overflow_total;
    out->accum_after = w->accum.sessions_overflow;
    out->records     = w->total_records_written;

    pgwt_summary_close(w);
    pgwt_summary_destroy(w);
    free(w);
}

static void test_sessions_overflow_is_reported(void)
{
    printf("--- 14. a full sessions[] table is REPORTED, not dropped (#555) ---\n");

    const int EXTRA = 50;

    /* 14a. NORMAL captures. The demo's shape (4 pids) and the bound itself
     * (exactly 1024, which open addressing must place in full). This is the
     * SAME `sessions_overflow == 0` assertion that must go red in 14b. */
    struct sess_probe p4;
    sess_probe_run("sess_normal", 4, 1, EV_LOCK, &p4);
    CHECK(p4.opened == 1 && p4.placed == 4 && p4.records == 1 &&
          p4.class_ns == 4 * SESS_ONE_NS,
          "the 4-pid fixture RAN (open=%d placed=%d records=%llu "
          "class_ns=%llu) -- without this the zero below is untested",
          p4.opened, p4.placed, (unsigned long long)p4.records,
          (unsigned long long)p4.class_ns);
    CHECK(p4.sessions_ns == p4.class_ns,
          "...Sigma sessions[].db_time_ns == Sigma class_ns (%llu == %llu): "
          "nothing was lost",
          (unsigned long long)p4.sessions_ns,
          (unsigned long long)p4.class_ns);
    CHECK(p4.overflow == 0 && p4.total_after == 0,
          "...and sessions_overflow is 0 (record %u, total %llu): the counter "
          "does not free-run on a capture that fits",
          p4.overflow, (unsigned long long)p4.total_after);

    struct sess_probe pfull;
    sess_probe_run("sess_at_bound", SUMMARY_MAX_SESSIONS, 1, EV_LOCK,
                   &pfull);
    CHECK(pfull.placed == SUMMARY_MAX_SESSIONS &&
          pfull.class_ns == (uint64_t)SUMMARY_MAX_SESSIONS * SESS_ONE_NS,
          "EXACTLY at the bound all %d pids are placed (%d, class_ns=%llu)",
          SUMMARY_MAX_SESSIONS, pfull.placed,
          (unsigned long long)pfull.class_ns);
    CHECK(pfull.sessions_ns == pfull.class_ns,
          "...and nothing is lost (%llu == %llu)",
          (unsigned long long)pfull.sessions_ns,
          (unsigned long long)pfull.class_ns);
    CHECK(pfull.overflow == 0 && pfull.total_after == 0,
          "...and the counter is STILL 0 (record %u, total %llu) -- an "
          "off-by-one in the bound would show here and nowhere else",
          pfull.overflow, (unsigned long long)pfull.total_after);

    /* 14b. THE OVERFLOW. 1024 + 50 distinct pids in one second. */
    struct sess_probe pover;
    sess_probe_run("sess_overflow", SUMMARY_MAX_SESSIONS + EXTRA, 1,
                   EV_LOCK, &pover);
    CHECK(pover.placed == SUMMARY_MAX_SESSIONS &&
          pover.class_ns ==
              (uint64_t)(SUMMARY_MAX_SESSIONS + EXTRA) * SESS_ONE_NS,
          "the fixture really overflowed: %d of %d pids placed while class_ns "
          "kept all %d ms (%llu ns)",
          pover.placed, SUMMARY_MAX_SESSIONS + EXTRA,
          SUMMARY_MAX_SESSIONS + EXTRA, (unsigned long long)pover.class_ns);
    CHECK(pover.sessions_ns == (uint64_t)SUMMARY_MAX_SESSIONS * SESS_ONE_NS,
          "...sessions[] carries only the %d it could hold (%llu ns): this is "
          "the shortfall a reader sees as Sigma Sessions < DB Time",
          SUMMARY_MAX_SESSIONS, (unsigned long long)pover.sessions_ns);
    CHECK(pover.overflow == (uint32_t)EXTRA,
          "...and sessions_overflow is EXACTLY the %d denied pids (got %u). "
          "`> 0` would also pass for a counter that ticks per event, per "
          "flush, or always -- which is why this is an equality",
          EXTRA, pover.overflow);
    CHECK(pover.class_ns - pover.sessions_ns ==
              (uint64_t)pover.overflow * SESS_ONE_NS,
          "...and it accounts for the WHOLE shortfall: class_ns - Sigma "
          "sessions[] = %llu ns = %u x 1 ms. The two sides come from "
          "different accumulators -- class_ns never consults sessions[] -- so "
          "this cannot be satisfied by one number compared with itself",
          (unsigned long long)(pover.class_ns - pover.sessions_ns),
          pover.overflow);
    CHECK(pover.total_after == (uint64_t)EXTRA,
          "...the count REACHES the writer total the control socket reports "
          "as summary_sessions_overflow_total (%llu, expected %d)",
          (unsigned long long)pover.total_after, EXTRA);
    CHECK(pover.accum_after == 0,
          "...and the per-second field is cleared by the flush (%u), so the "
          "next second cannot re-add this one's drops",
          pover.accum_after);

    /* 14c. THE UNIT IS ONE DENIED LOOKUP, not one denied pid: two pushes per
     * pid must double the count, and the ns shortfall doubles with it. Pins
     * what the field means, and is a second way `> 0` is not enough. */
    struct sess_probe ptwice;
    sess_probe_run("sess_overflow_x2", SUMMARY_MAX_SESSIONS + EXTRA, 2,
                   EV_LOCK, &ptwice);
    CHECK(ptwice.overflow == (uint32_t)(2 * EXTRA),
          "two pushes per pid -> %d denied LOOKUPS (got %u), not %d denied "
          "pids: the counter's unit is the same one events_overflow uses",
          2 * EXTRA, ptwice.overflow, EXTRA);
    CHECK(ptwice.class_ns - ptwice.sessions_ns ==
              (uint64_t)ptwice.overflow * SESS_ONE_NS,
          "...and the shortfall doubles with it (%llu ns)",
          (unsigned long long)(ptwice.class_ns - ptwice.sessions_ns));

    /* 14g. THE COUNTER'S MEANING: a denial that COST sessions[] something.
     *
     * Found in review. The accum_event increment used to be unconditional,
     * but every write into a session slot already excludes idle events
     * (db_time_ns and top_wait_* by pgwt_is_idle_event, cpu_ns by
     * `old_ev == 0`, the raw on-CPU id, which is never idle). So a slot
     * denied to an idle-only pid loses NOTHING -- and the WARN and the
     * control-socket metric both assert "Top Sessions sums to LESS than DB
     * Time", which was then a false alarm on exactly the workload that trips
     * the bound: an instance with >1024 connections is mostly backends
     * sitting in Client:ClientRead.
     *
     * Reproduced before it was fixed: 1024 + 50 ClientRead-only pids gave
     * sessions_overflow = 50 with class_ns = 0 and Sigma sessions[] = 0, a
     * 0 ns gap against an invariant demanding 50 x 1 ms.
     *
     * 14g pins the choice in BOTH directions, because a guard that simply
     * stopped counting would also pass an "idle denials are 0" assertion by
     * never counting anything. So: idle-only must be 0 WHILE THE TABLE IS
     * DEMONSTRABLY FULL and the idle time demonstrably arrived, and a mixed
     * second must count the non-idle denials and only those. */
    struct sess_probe pidle;
    sess_probe_run("sess_idle_overflow", SUMMARY_MAX_SESSIONS + EXTRA, 1,
                   EV_CLIENTREAD, &pidle);
    CHECK(pidle.placed == SUMMARY_MAX_SESSIONS &&
          pidle.idle_ns ==
              (uint64_t)(SUMMARY_MAX_SESSIONS + EXTRA) * SESS_ONE_NS,
          "the idle fixture really DID overflow the table and really DID "
          "arrive: %d of %d pids placed, idle_ns=%llu (expected %llu). "
          "Without these two the zero below is a gate that stopped counting",
          pidle.placed, SUMMARY_MAX_SESSIONS + EXTRA,
          (unsigned long long)pidle.idle_ns,
          (unsigned long long)((uint64_t)(SUMMARY_MAX_SESSIONS + EXTRA) *
                               SESS_ONE_NS));
    CHECK(pidle.class_ns == 0 && pidle.sessions_ns == 0,
          "...and NOTHING was lost from sessions[]: class_ns=%llu and Sigma "
          "sessions[]=%llu are both 0, because idle time enters neither",
          (unsigned long long)pidle.class_ns,
          (unsigned long long)pidle.sessions_ns);
    CHECK(pidle.overflow == 0 && pidle.total_after == 0,
          "...so sessions_overflow is 0 (record %u, total %llu). It was %d "
          "before this guard, and the WARN and the metric it fires both "
          "assert a DB-time shortfall that does not exist -- a false alarm on "
          "the one workload that reaches the bound",
          pidle.overflow, (unsigned long long)pidle.total_after, EXTRA);

    /* 14g-ii. THE MIXED SECOND, which is what a real >1024-connection
     * instance looks like: the table filled by working backends, then both
     * idle and non-idle pids denied in the same second. The count must be the
     * non-idle denials and ONLY those -- this is the assertion a guard that
     * stopped counting altogether cannot pass. */
    {
        const int IDLE_DENIED = 50, LOAD_DENIED = 30;
        const char *dir = fresh_dir("sess_mixed");
        struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
        CHECK(w != NULL && pgwt_summary_writer_init(w, dir, 24, NULL) == 0,
              "mixed-fixture writer opens");
        if (w) {
            uint64_t sec = mono_origin();
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            e.new_event = EV_IO; e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
            e.timestamp_ns = sec + 100 * MS;
            e.duration_ns = SESS_ONE_NS;
            e.query_id = 0;
            /* fill the table with non-idle backends */
            e.old_event = EV_LOCK;
            for (int k = 0; k < SUMMARY_MAX_SESSIONS; k++) {
                e.pid = (uint32_t)(9000 + k);
                pgwt_summary_push_event(w, &e, 0, e.old_event);
            }
            CHECK(w->accum.num_sessions == SUMMARY_MAX_SESSIONS &&
                  w->accum.sessions_overflow == 0,
                  "the table is full with nothing denied yet (%d placed, "
                  "overflow %u)", w->accum.num_sessions,
                  w->accum.sessions_overflow);
            /* now deny: idle pids first, then non-idle ones */
            e.old_event = EV_CLIENTREAD;
            for (int k = 0; k < IDLE_DENIED; k++) {
                e.pid = (uint32_t)(20000 + k);
                pgwt_summary_push_event(w, &e, 0, e.old_event);
            }
            CHECK(w->accum.sessions_overflow == 0,
                  "%d IDLE pids denied a slot add nothing to the counter "
                  "(%u), because a slot would have given them nothing",
                  IDLE_DENIED, w->accum.sessions_overflow);
            e.old_event = EV_LOCK;
            for (int k = 0; k < LOAD_DENIED; k++) {
                e.pid = (uint32_t)(30000 + k);
                pgwt_summary_push_event(w, &e, 0, e.old_event);
            }
            CHECK(w->accum.sessions_overflow == (uint32_t)LOAD_DENIED,
                  "...and the %d NON-IDLE pids denied in the SAME second are "
                  "counted, exactly (%u). A guard that merely stopped "
                  "counting fails here; the idle zero above alone would not "
                  "have caught it",
                  LOAD_DENIED, w->accum.sessions_overflow);

            uint64_t sess_ns = 0, cls_ns = 0;
            for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++)
                sess_ns += w->accum.sessions[i].db_time_ns;
            for (int i = 0; i < PGWT_NUM_CLASSES; i++)
                cls_ns += w->accum.class_ns[i];
            CHECK(cls_ns - sess_ns ==
                      (uint64_t)w->accum.sessions_overflow * SESS_ONE_NS,
                  "...and the counter still accounts for the WHOLE DB-time "
                  "shortfall in a second that also dropped idle pids: "
                  "class_ns - Sigma sessions[] = %llu ns = %u x 1 ms, with "
                  "the %d idle drops contributing to neither side",
                  (unsigned long long)(cls_ns - sess_ns),
                  w->accum.sessions_overflow, IDLE_DENIED);
            CHECK(w->accum.idle_ns == (uint64_t)IDLE_DENIED * SESS_ONE_NS,
                  "...while the exact idle scalar still carries every denied "
                  "idle pid's %d ms (%llu ns): their time is not lost, it was "
                  "never sessions[] data",
                  IDLE_DENIED, (unsigned long long)w->accum.idle_ns);

            pgwt_summary_flush(w);
            CHECK(w->sessions_overflow_total == (uint64_t)LOAD_DENIED,
                  "...and only those %d reach the writer total (%llu)",
                  LOAD_DENIED, (unsigned long long)w->sessions_overflow_total);
            pgwt_summary_close(w);
            pgwt_summary_destroy(w);
            free(w);
        }
    }

    /* 14d. THE ROLL-UP IS NOT DOUBLE-COUNTED across a failed flush and its
     * retry -- the identical hazard section 11 pins for events_overflow. A
     * failed flush leaves the accumulator OPEN, so a roll-up placed before
     * the early returns would add this second's drops once per attempt and a
     * counter that inflates under failure reads as a second, independent
     * problem. Poked directly, exactly as section 11 does. */
    {
        const char *dir = fresh_dir("sess_retry");
        struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
        CHECK(w != NULL && pgwt_summary_writer_init(w, dir, 24, NULL) == 0,
              "retry writer opens");
        if (w) {
            uint64_t sec = mono_origin();
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            e.new_event = EV_IO; e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
            e.pid = 970; e.timestamp_ns = sec + 100 * MS;
            e.old_event = EV_LOCK; e.duration_ns = 10 * MS;
            pgwt_summary_push_event(w, &e, 0, e.old_event);
            CHECK(w->accum_active, "a second is open");

            w->accum.sessions_overflow = 7;
            uint64_t before = w->sessions_overflow_total;

            size_t real = w->encode_buf_size;
            w->encode_buf_size = PGWT_SUMMARY_SERIALIZE_MAX - 1;
            int rc = pgwt_summary_flush(w);
            w->encode_buf_size = real;
            CHECK(rc != 0 && w->accum_active,
                  "the flush FAILS and leaves the second open (rc=%d)", rc);
            CHECK(w->sessions_overflow_total == before,
                  "...and a second that was never written adds NOTHING to the "
                  "total (%llu == %llu) -- same rule as "
                  "events_overflow_total: written seconds only",
                  (unsigned long long)w->sessions_overflow_total,
                  (unsigned long long)before);
            CHECK(w->accum.sessions_overflow == 7,
                  "...the per-second count survives for the retry (%u)",
                  w->accum.sessions_overflow);

            int rc2 = pgwt_summary_flush(w);
            CHECK(rc2 == 0, "the retry succeeds (rc=%d)", rc2);
            CHECK(w->sessions_overflow_total == before + 7,
                  "...and the 7 drops are counted ONCE across the failed "
                  "attempt and its retry (%llu, expected %llu)",
                  (unsigned long long)w->sessions_overflow_total,
                  (unsigned long long)(before + 7));

            pgwt_summary_close(w);
            pgwt_summary_destroy(w);
            free(w);
        }
    }

    /* 14e. THE READ SIDE. pgwt_summary_deserialize drops a session entry the
     * table cannot take, and that path is reachable only from a CORRUPT or
     * FORGED block (the serializer emits at most SUMMARY_MAX_SESSIONS
     * entries) -- which is the case where silence is worst, because the
     * decode SUCCEEDS and the window is then served and labelled exact. A
     * payload with 1024 + 8 session entries must decode, drop 8, and SAY so.
     *
     * AND THE COUNTER ALONE IS NOT "SAYING SO", which the first cut of this
     * change claimed. Found in review: acc->sessions_overflow is not
     * serialized YET (the approved PGWT_SUMMARY_VERSION 8 -> 9 bump puts it
     * on disk; it is not in this change), no production code reads it, and
     * pgwt-server instantiates no summary_writer -- so the roll-up's only
     * consumer (control.c's metrics reply) does not exist in the process
     * that decodes blocks. Note the assertions below stay correct AFTER that
     * bump: they pin the READER's own count, which the trailer read must add
     * to rather than overwrite (summary_writer.h names that trap). The
     * drop site therefore emits a one-per-process WARN, and this section
     * asserts the WARN, not just the field. The one-shot is asserted too: a
     * reader walking a corrupt FILE must not emit one line per second of the
     * window.
     *
     * The payload is built by hand rather than serialized, for the same
     * reason section 9 builds its old-layout bytes by hand: the writer
     * cannot produce this record. Layout for PGWT_SUMMARY_VERSION:
     * class_ns | sessions | queries | v4 trailer (12) | v7 trailer (40). */
    {
        const int OVER = 8;
        const int NSESS = SUMMARY_MAX_SESSIONS + OVER;
        /* v9 session entry: 4+8+8+4+8 + v9's 8+4. */
        size_t sz = (size_t)PGWT_NUM_CLASSES * 8 + (size_t)NSESS * 44 + 12 + 40;
        uint8_t *buf = calloc(1, sz);
        struct pgwt_summary_accum *back = calloc(1, sizeof(*back));
        CHECK(buf != NULL && back != NULL, "alloc the forged payload");
        if (buf && back) {
            uint8_t *p = buf;
            uint64_t cls = (uint64_t)NSESS * SESS_ONE_NS;
            memcpy(p + PGWT_CLASS_LOCK * 8, &cls, 8);
            p += PGWT_NUM_CLASSES * 8;
            for (int k = 0; k < NSESS; k++) {
                uint32_t pid = (uint32_t)(7000 + k);
                uint64_t ns = SESS_ONE_NS;
                memcpy(p, &pid, 4);  p += 4;   /* pid          */
                memcpy(p, &ns, 8);   p += 8;   /* db_time_ns   */
                p += 8;                        /* cpu_ns     0 */
                p += 4;                        /* top_wait_id  */
                p += 8;                        /* top_wait_ns  */
                p += 8;                        /* v9 cpu_offcpu_ns 0 */
                p += 4;                        /* v9 cpu_measured_records 0 */
            }
            p += 12;   /* v4 trailer: idle_ns + events_overflow, both 0 */
            p += 40;   /* v7 measured-CPU trailer, all 0                */
            CHECK((size_t)(p - buf) == sz,
                  "the forged payload is the %zu bytes the reader expects "
                  "(wrote %zu)", sz, (size_t)(p - buf));

            back->num_events   = 0;
            back->num_sessions = NSESS;   /* what the corrupt header claims */
            back->num_queries  = 0;

            /* Capture the reader's stderr across TWO forged decodes and
             * COUNT the WARN, rather than splitting the capture at an offset.
             *
             * The first cut of this case did split it, with ftell() after the
             * first decode, and that assertion was VACUOUS: the mutant that
             * makes the WARN fire per dropped entry emits 16 lines, the
             * capture overran the 2 KB buffer, `split > bytes_read` skipped
             * the comparison, and the one-shot check passed on the mutant it
             * existed to catch. Counting occurrences over the whole window has
             * no offset to get wrong, and the buffer is checked for
             * truncation below so a flood REFUSES instead of approving. */
            char errlog[700], cap_txt[8192] = "";
            size_t cap_len = 0;
            const char *WARN_MARK = "more sessions than the table holds";
            snprintf(errlog, sizeof(errlog), "%s/sess_reader_warn.txt",
                     g_base);
            int rc = -1, rc2 = -1, truncated = 1;
            fflush(stderr);
            int saved_err = dup(STDERR_FILENO);
            FILE *cap = fopen(errlog, "w+");
            CHECK(saved_err >= 0 && cap != NULL,
                  "captured the reader's stderr (fd=%d)", saved_err);
            if (saved_err >= 0 && cap) {
                dup2(fileno(cap), STDERR_FILENO);
                rc = pgwt_summary_deserialize(buf, sz, back,
                                              PGWT_SUMMARY_VERSION);
                /* A SECOND forged decode inside the SAME window. */
                struct pgwt_summary_accum *again = calloc(1, sizeof(*again));
                if (again) {
                    again->num_sessions = NSESS;
                    rc2 = pgwt_summary_deserialize(buf, sz, again,
                                                   PGWT_SUMMARY_VERSION);
                    free(again);
                }
                fflush(stderr);
                dup2(saved_err, STDERR_FILENO);
                close(saved_err);
                rewind(cap);
                cap_len = fread(cap_txt, 1, sizeof(cap_txt) - 1, cap);
                cap_txt[cap_len] = 0;
                truncated = (cap_len >= sizeof(cap_txt) - 1);
                fclose(cap);
            }
            int warn_lines = 0;
            for (const char *q = cap_txt;
                 (q = strstr(q, WARN_MARK)) != NULL; q += 1)
                warn_lines++;

            uint64_t got = 0;
            for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++)
                got += back->sessions[i].db_time_ns;

            CHECK(rc == 0,
                  "the forged record DECODES SUCCESSFULLY (rc=%d) -- that is "
                  "the hazard: a decode error would be skipped and counted, "
                  "this one is served", rc);
            CHECK(back->num_sessions == SUMMARY_MAX_SESSIONS &&
                  got == (uint64_t)SUMMARY_MAX_SESSIONS * SESS_ONE_NS,
                  "...it placed %d of %d entries and %llu ns of %llu",
                  back->num_sessions, NSESS, (unsigned long long)got,
                  (unsigned long long)cls);
            CHECK(back->sessions_overflow == (uint32_t)OVER,
                  "...the READER counted EXACTLY the %d entries it dropped "
                  "(%u) -- in a field that reaches nobody on this side, which "
                  "is why the WARN below is the part that makes the decode "
                  "non-silent",
                  OVER, back->sessions_overflow);
            CHECK(!truncated,
                  "the stderr capture is COMPLETE (%zu of %zu bytes) -- a "
                  "full buffer must refuse, because that is how the earlier "
                  "version of the one-shot check below passed on a WARN that "
                  "fired 16 times", cap_len, sizeof(cap_txt) - 1);
            CHECK(rc2 == 0,
                  "...the second forged decode also succeeded (rc=%d), so "
                  "both decodes really did reach the drop branch", rc2);
            CHECK(warn_lines == 1,
                  "...and the drop is REPORTED on stderr EXACTLY ONCE across "
                  "both decodes and all %d dropped entries (%d lines). "
                  "0 means the read side is still silent -- the counter it "
                  "sets reaches nobody on this side; >1 means a corrupt FILE "
                  "would emit a line per entry per second and bury the "
                  "operator. Capture: %.160s",
                  2 * OVER, warn_lines,
                  cap_txt[0] ? cap_txt : "<nothing at all>");
            CHECK(back->class_ns[PGWT_CLASS_LOCK] - got ==
                      (uint64_t)back->sessions_overflow * SESS_ONE_NS,
                  "...matching the shortfall against the class_ns the SAME "
                  "decode produced (%llu ns)",
                  (unsigned long long)(back->class_ns[PGWT_CLASS_LOCK] - got));
        }
        free(buf);
        free(back);
    }

    /* 14f. FALSE NEGATIVES: the ways this gate could be satisfied without
     * checking anything.
     *
     * (i) AN EMPTY FIXTURE satisfies `sessions_overflow == 0` while
     * exercising nothing -- the shape in which a gate approves because it
     * cannot see. So the zero-pid probe is asserted to be RECOGNISABLY
     * empty: no second opened, no class time, no record written. 14a's zeros
     * are evidence only because the same probe reports a positive class_ns
     * and a written record, which this case shows is a distinguishable
     * state. */
    struct sess_probe p0;
    sess_probe_run("sess_empty", 0, 1, EV_LOCK, &p0);
    CHECK(p0.opened == 0 && p0.class_ns == 0 && p0.records == 0 &&
          p0.sessions_ns == 0,
          "an empty fixture is RECOGNISABLY empty (open=%d class_ns=%llu "
          "records=%llu) -- so a zero counter on it is never mistaken for a "
          "tested zero",
          p0.opened, (unsigned long long)p0.class_ns,
          (unsigned long long)p0.records);
    CHECK(p0.overflow == 0 && p0.total_after == 0,
          "...and it reports no overflow (%u / %llu), which is only "
          "meaningful next to 14b's %d",
          p0.overflow, (unsigned long long)p0.total_after, EXTRA);

    /* (ii) A WRITER WHOSE SECONDS NEVER REACH DISK. The counter alone
     * cannot tell "this capture fit in 1024 pids" from "nothing was ever
     * written", and the second is how a gate reports success while blind. The
     * witnesses are `records` and the flush status, and this case proves they
     * really do separate the two: trace_dir is a regular FILE, so the
     * writer's fopen fails with ENOTDIR for any uid (a mode-0555 directory
     * would not stop the root the gate box runs as). The second is lost, so
     * its drops are NOT rolled up -- the same written-seconds-only rule
     * events_overflow_total carries -- and the honest verdict is NOT RUN. */
    {
        char filepath[700];
        snprintf(filepath, sizeof(filepath), "%s/sess_notadir", g_base);
        FILE *touch = fopen(filepath, "w");
        CHECK(touch != NULL, "created a FILE where trace_dir should be");
        if (touch) fclose(touch);

        struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
        CHECK(w != NULL && pgwt_summary_writer_init(w, filepath, 24, NULL) == 0,
              "the writer comes up (it opens no file until the first flush)");
        if (w) {
            uint64_t sec = mono_origin();
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            e.new_event = EV_IO; e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
            e.pid = 980; e.timestamp_ns = sec + 100 * MS;
            e.old_event = EV_LOCK; e.duration_ns = 10 * MS;
            pgwt_summary_push_event(w, &e, 0, e.old_event);
            w->accum.sessions_overflow = 5;   /* this second DID drop pids */
            int rc = pgwt_summary_flush(w);
            CHECK(rc != 0 || w->total_records_written == 0,
                  "the second cannot be written (rc=%d, records=%llu)",
                  rc, (unsigned long long)w->total_records_written);
            CHECK(w->sessions_overflow_total == 0,
                  "...so its 5 drops are NOT in the total (%llu): written "
                  "seconds only. The zero here is NOT a pass -- records=%llu "
                  "and flush_failures_total=%llu are what say so, and a reader "
                  "must pair the counter with them",
                  (unsigned long long)w->sessions_overflow_total,
                  (unsigned long long)w->total_records_written,
                  (unsigned long long)w->flush_failures_total);
            CHECK(w->flush_failures_total > 0 || !w->enabled,
                  "...and the failure is itself REPORTED "
                  "(flush_failures_total=%llu, enabled=%d), so the window is "
                  "known to have a hole rather than looking complete",
                  (unsigned long long)w->flush_failures_total, w->enabled);
            pgwt_summary_destroy(w);
        }
        free(w);
    }
}

/* ── 15. measured CPU inside a VISIBLE idle interval (#681, the #194 gate) ──
 * All 16 original cpu_ns assignments were UNKNOWN. They could not see #194
 * crediting the protocol tail of ClientRead to CPU on raw alone: both tiers
 * currently discard that measurement. Keep this stream separate from slice[]
 * and build_stream(): 130 seconds each carry 40 ms idle / 12 ms measured CPU,
 * a 50 ms CPU gap / 30 ms measured CPU, and a 20 ms UNKNOWN Lock wait.
 * The 1560 ms at risk is 17.142857 pp of this fixture's 9100 ms DB Time.
 * Every new ms comparison is exact: all contributions are integer ns divided
 * by 1e6, with no window-edge clipping or second-boundary straddlers. */

struct measured_idle_probe {
    struct pgwt_tm_result raw, sum;
    uint64_t measured_idle_ns;
};

#define IDLE_CPU_REFUSAL(a) (1U << ((a) - 1))

/* Run the SAME A gate for the baseline and every bypass. Expected refusals
 * are observations, not failed parity checks; callers must assert the reason.
 * Collect all reasons in A1..A7 order: zero CPU fails A3's positive sum AND
 * A4's strict-partial requirement, so D2 must explicitly witness A4 too. */
static unsigned measured_idle_gate(const char *label, const char *dir,
                                   const struct pgwt_trace_event *ev, int n,
                                   int accepted, uint64_t origin,
                                   uint64_t committed_from,
                                   uint64_t from, uint64_t to,
                                   int expect_refusal,
                                   struct measured_idle_probe *p)
{
    struct rec_ctx c;
    memset(&c, 0, sizeof(c));
    int read = pgwt_visit_summaries(dir, from, to, rec_visitor, &c);
    uint64_t span = to > from ? to - from : 0;
    /* Translate the COMMITTED wall window back to this one monotonic origin,
     * including D5's displaced window; never take another clock reading. */
    uint64_t raw_from = origin + (from - committed_from);
    uint64_t raw_to = raw_from + span;
    double wall_ms = (double)span / 1e6;
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    memset(p, 0, sizeof(*p));
    pgwt_compute_time_model(ev, n, &f, raw_from, raw_to, wall_ms, &p->raw);
    pgwt_compute_time_model_from_summaries(dir, from, to, &f, wall_ms,
                                          &p->sum);

    int measured = 0, invalid = 0;
    for (int i = 0; i < n; i++) {
        const struct pgwt_trace_event *e = &ev[i];
        if (e->timestamp_ns <= raw_from || e->timestamp_ns > raw_to ||
            !pgwt_is_idle_event(e->old_event) ||
            e->cpu_ns == PGWT_CPU_NS_UNKNOWN)
            continue;
        measured++;
        p->measured_idle_ns += e->cpu_ns;
        if (e->cpu_ns == 0 || e->cpu_ns >= e->duration_ns)
            invalid++;
    }
    double measured_ms = (double)p->measured_idle_ns / 1e6;
    double pp = p->sum.db_time_ms > 0
                ? measured_ms / p->sum.db_time_ms * 100.0 : 0.0;
    int considered = -1, unusable = -1;
    unsigned refused = 0;

#define IDLE_CPU_REQUIRE(a, cond, fmt, ...) do {                          \
    int ok = (cond);                                                     \
    if (!expect_refusal)                                                 \
        CHECK(ok, "%s: REFUSED A%d: " fmt, label, a, ##__VA_ARGS__);       \
    if (!ok) {                                                          \
        refused |= IDLE_CPU_REFUSAL(a);                                  \
        printf("  %s: REFUSED A%d: " fmt "\n", label, a, ##__VA_ARGS__);   \
    }                                                                   \
} while (0)

    IDLE_CPU_REQUIRE(1, accepted == n && n > 0 && read > 0 &&
                        c.records == 130 && c.events_overflow == 0,
                     "accepted %d/%d pushes, reader %d, records %d/130, "
                     "overflow %u", accepted, n, read, c.records,
                     c.events_overflow);
    IDLE_CPU_REQUIRE(2, span >= 120ULL * ONE_SEC,
                     "window %.6f s (requires >= 120 s)",
                     (double)span / 1e9);
    IDLE_CPU_REQUIRE(3, p->measured_idle_ns > 0 && measured > 0,
                     "measured idle CPU %.6f ms in %d records (both > 0)",
                     measured_ms, measured);
    IDLE_CPU_REQUIRE(4, invalid == 0,
                     "%d/%d measured idle records violate 0 < cpu_ns < "
                     "duration_ns (measured %.6f ms)", invalid, measured,
                     measured_ms);
    IDLE_CPU_REQUIRE(5, p->raw.idle_time_ms > 0 && p->sum.idle_time_ms > 0 &&
                        p->raw.idle_time_ms == 40.0 * 130 &&
                        p->sum.idle_time_ms == 40.0 * 130,
                     "Idle raw %.6f / summary %.6f ms, fixture 5200 ms; "
                     "measured CPU inside the visible idle intervals %.6f ms",
                     p->raw.idle_time_ms, p->sum.idle_time_ms, measured_ms);
    IDLE_CPU_REQUIRE(6, pp > 0.0 && p->raw.db_time_ms > 0,
                     "blast radius %.6f pp of DB Time "
                     "(%.6f ms measured / %.6f ms summary, raw %.6f ms)",
                     pp, measured_ms, p->sum.db_time_ms, p->raw.db_time_ms);
    int verdict = pgwt_summaries_window_current(dir, from, to, &considered,
                                                &unusable);
    IDLE_CPU_REQUIRE(7, verdict == 1 && considered > 0 && unusable == 0,
                     "preflight verdict %d, considered %d, unusable %d",
                     verdict, considered, unusable);
#undef IDLE_CPU_REQUIRE

    if (refused == 0) {
        printf("  %s: accepted %d/%d pushes, records %d, overflow %u; "
               "window %.6f s; measured idle CPU %.6f ms in %d records, "
               "%d invalid measurements\n", label, accepted, n, c.records,
               c.events_overflow, (double)span / 1e9, measured_ms, measured,
               invalid);
        printf("  %s: Idle raw %.6f / summary %.6f ms (fixture 5200 ms); "
               "blast radius %.6f pp of DB Time; preflight verdict %d, "
               "considered %d, unusable %d\n", label, p->raw.idle_time_ms,
               p->sum.idle_time_ms, pp, verdict, considered, unusable);
    }
    return refused;
}

static void test_measured_cpu_inside_visible_idle(void)
{
    printf("--- 15. measured CPU inside a VISIBLE idle interval "
           "(#681, the #194 gate) ---\n");
    enum { SECONDS = 130, EVENTS = SECONDS * 3 };
    struct pgwt_trace_event ev[EVENTS], credited[SECONDS * 4];
    uint64_t origin = mono_origin();
    int n = 0;
    for (int s = 0; s < SECONDS; s++) {
        uint64_t sec = origin + (uint64_t)s * ONE_SEC;
        struct pgwt_trace_event e;
        memset(&e, 0, sizeof(e));
        e.pid = 981;
        e.query_id = QID_A;
        /* Contiguous intervals at [100,140), [140,190), [190,210) ms,
         * emitted by END timestamp: #277 must not fold a late event. No
         * category flag and no pre-relabelled non-command id (#408). */
        e.timestamp_ns = sec + 140 * MS;
        e.old_event = EV_CLIENTREAD; e.new_event = 0;
        e.duration_ns = 40 * MS; e.cpu_ns = 12 * MS;
        ev[n++] = e;
        e.timestamp_ns = sec + 190 * MS;
        e.old_event = 0; e.new_event = EV_LOCK;
        e.duration_ns = 50 * MS; e.cpu_ns = 30 * MS;
        ev[n++] = e;
        e.timestamp_ns = sec + 210 * MS;
        e.old_event = EV_LOCK; e.new_event = EV_CLIENTREAD;
        e.duration_ns = 20 * MS; e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
        ev[n++] = e;
    }

    char dir[600];
    snprintf(dir, sizeof(dir), "%s", fresh_dir("measured_visible_idle"));
    int accepted = write_summaries(dir, ev, n);
    uint64_t from = 0, to = 0;
    int records = 0;
    int have_window = window_from_records(dir, &from, &to, &records);
    CHECK(have_window && records > 0,
          "REFUSED A1: committed window available=%d, records=%d",
          have_window, records);
    if (!have_window) return;

    struct measured_idle_probe p;
    unsigned refused = measured_idle_gate("15 A", dir, ev, n, accepted,
                                          origin, from, from, to, 0, &p);
    if (refused) {
        free(p.raw.rows);
        free(p.sum.rows);
        return;                     /* a blind gate never reaches parity */
    }

    /* B. The #194 gate stays RED on a real one-sided change. CPU rows must
     * exist as measured, non-zero quantities before their equality matters. */
    double measured_ms = (double)p.measured_idle_ns / 1e6;
    CHECK(p.raw.db_time_ms == p.sum.db_time_ms,
          "DB Time raw %.6f / summary %.6f ms: the measured CPU inside the "
          "visible idle intervals (%.6f ms) is credited the same way by "
          "both tiers", p.raw.db_time_ms, p.sum.db_time_ms, measured_ms);
    CHECK(p.raw.idle_time_ms == p.sum.idle_time_ms,
          "Idle raw %.6f / summary %.6f ms: the measured CPU inside the "
          "visible idle intervals (%.6f ms) is credited the same way by "
          "both tiers", p.raw.idle_time_ms, p.sum.idle_time_ms, measured_ms);
    CHECK(p.raw.has_measured_cpu && p.sum.has_measured_cpu,
          "CPU split is measured on both tiers (%d/%d)",
          p.raw.has_measured_cpu, p.sum.has_measured_cpu);
    const char *cpu_rows[] = {"CPU (running)", "CPU (waiting for a core)"};
    for (int i = 0; i < 2; i++) {
        const struct pgwt_tm_row *r = row_at(&p.raw, cpu_rows[i], 1);
        const struct pgwt_tm_row *s = row_at(&p.sum, cpu_rows[i], 1);
        CHECK(r && s && r->time_ms > 0 && s->time_ms > 0,
              "%s exists and is positive: raw %.6f / summary %.6f ms",
              cpu_rows[i], r ? r->time_ms : 0.0, s ? s->time_ms : 0.0);
        if (r && s)
            CHECK(r->time_ms == s->time_ms,
                  "%s raw %.6f / summary %.6f ms: the measured CPU inside "
                  "the visible idle intervals (%.6f ms) is credited the "
                  "same way by both tiers", cpu_rows[i], r->time_ms,
                  s->time_ms, measured_ms);
    }

    /* C. Credit ONLY raw's 12 ms prefix, leaving 28 ms of ClientRead.
     * Summary input and answer stay unchanged. There is no PROVOKE idiom
     * in this file: assert the inequalities directly instead of blessing a
     * raw-vs-summary disagreement in the baseline above. */
    int nc = 0;
    for (int i = 0; i < n; i++) {
        struct pgwt_trace_event e = ev[i];
        if (e.old_event == EV_CLIENTREAD) {
            struct pgwt_trace_event cpu = e;
            cpu.timestamp_ns = e.timestamp_ns - e.duration_ns + e.cpu_ns;
            cpu.old_event = 0;
            cpu.new_event = EV_CLIENTREAD;
            cpu.duration_ns = e.cpu_ns;
            credited[nc++] = cpu;
            e.duration_ns -= e.cpu_ns;
            e.cpu_ns = 0;           /* all 12 measured ms moved to the prefix */
        }
        credited[nc++] = e;
    }
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_tm_result control;
    pgwt_compute_time_model(credited, nc, &f, origin, origin + (to - from),
                             (double)(to - from) / 1e6, &control);
    CHECK(control.db_time_ms != p.sum.db_time_ms &&
          control.idle_time_ms != p.sum.idle_time_ms,
          "C1: raw-only #194 credit must DIVERGE: DB Time %.6f/%.6f, "
          "Idle %.6f/%.6f ms", control.db_time_ms, p.sum.db_time_ms,
          control.idle_time_ms, p.sum.idle_time_ms);
    CHECK(control.db_time_ms - p.sum.db_time_ms == measured_ms,
          "C2: DB Time higher by exactly %.6f ms (got %.6f ms)",
          measured_ms, control.db_time_ms - p.sum.db_time_ms);
    CHECK(p.sum.idle_time_ms - control.idle_time_ms == measured_ms,
          "C2: Idle lower by exactly %.6f ms (got %.6f ms)",
          measured_ms, p.sum.idle_time_ms - control.idle_time_ms);
    CHECK(control.cpu_ms - p.sum.cpu_ms == measured_ms,
          "C2: CPU higher by exactly %.6f ms (got %.6f ms)",
          measured_ms, control.cpu_ms - p.sum.cpu_ms);
    printf("  C3: raw-only #194 credit: DB Time +%.6f ms, Idle -%.6f ms, "
           "%.6f pp of unchanged summary DB Time\n",
           control.db_time_ms - p.sum.db_time_ms,
           p.sum.idle_time_ms - control.idle_time_ms,
           (control.db_time_ms - p.sum.db_time_ms) / p.sum.db_time_ms * 100.0);
    free(control.rows);
    free(p.raw.rows);
    free(p.sum.rows);

    /* D1..D4. Copy the LOCAL stream, never the shared fixture. fresh_dir's
     * static buffer is copied because the baseline is still needed by D5/6. */
    const char *labels[] = {"D1 UNKNOWN idle CPU", "D2 zero idle CPU",
                            "D3 whole-wall idle CPU", "D4 no idle records"};
    const unsigned want[] = {
        IDLE_CPU_REFUSAL(3) | IDLE_CPU_REFUSAL(6),
        IDLE_CPU_REFUSAL(3) | IDLE_CPU_REFUSAL(4) | IDLE_CPU_REFUSAL(6),
        IDLE_CPU_REFUSAL(4),
        IDLE_CPU_REFUSAL(3) | IDLE_CPU_REFUSAL(5) | IDLE_CPU_REFUSAL(6),
    };
    for (int leg = 0; leg < 4; leg++) {
        struct pgwt_trace_event bypass[EVENTS];
        int nb = 0;
        for (int i = 0; i < n; i++) {
            struct pgwt_trace_event e = ev[i];
            if (e.old_event == EV_CLIENTREAD) {
                if (leg == 3) continue;
                e.cpu_ns = leg == 0 ? PGWT_CPU_NS_UNKNOWN :
                           leg == 1 ? 0 : e.duration_ns;
            }
            bypass[nb++] = e;
        }
        char name[64], bypass_dir[600];
        snprintf(name, sizeof(name), "measured_idle_bypass_%d", leg + 1);
        snprintf(bypass_dir, sizeof(bypass_dir), "%s", fresh_dir(name));
        int pushed = write_summaries(bypass_dir, bypass, nb);
        uint64_t bf = 0, bt = 0;
        int br = 0;
        int window = window_from_records(bypass_dir, &bf, &bt, &br);
        CHECK(window && br == SECONDS && pushed == nb,
              "%s setup: window=%d, records=%d, accepted=%d/%d",
              labels[leg], window, br, pushed, nb);
        if (!window) continue;
        refused = measured_idle_gate(labels[leg], bypass_dir, bypass, nb,
                                      pushed, origin, bf, bf, bt, 1, &p);
        CHECK(refused == want[leg],
              "%s must be REFUSED (reasons 0x%x, required 0x%x)",
              labels[leg], refused, want[leg]);
        free(p.raw.rows);
        free(p.sum.rows);
    }

    /* D5. Healthy files, but the reader sees no record an hour after the
     * committed end. D6 removes that SAME directory after a successful write.
     * Neither leg is allowed to evaluate a parity equality on empty output. */
    uint64_t far_from = to + 3600ULL * ONE_SEC;
    refused = measured_idle_gate("D5 empty reader window", dir, ev, n,
                                  accepted, origin, from, far_from,
                                  far_from + (to - from), 1, &p);
    CHECK((refused & IDLE_CPU_REFUSAL(1)) != 0,
          "D5 must be REFUSED by A1's record gate (reasons 0x%x)", refused);
    free(p.raw.rows);
    free(p.sum.rows);

    char remove_cmd[800];
    snprintf(remove_cmd, sizeof(remove_cmd), "rm -rf '%s'", dir);
    int removed = system(remove_cmd);
    struct stat st;
    CHECK(removed == 0 && stat(dir, &st) != 0 && access(dir, F_OK) != 0,
          "D6 removed the written summary directory %s", dir);
    refused = measured_idle_gate("D6 removed summary directory", dir, ev, n,
                                  accepted, origin, from, from, to, 1, &p);
    CHECK((refused & IDLE_CPU_REFUSAL(1)) != 0 &&
          (refused & IDLE_CPU_REFUSAL(7)) != 0,
          "D6 must be REFUSED by A1 and A7 (reasons 0x%x)", refused);
    free(p.raw.rows);
    free(p.sum.rows);
}
#undef IDLE_CPU_REFUSAL

/* ── 16. v9 session measurements survive the format ───────────────────── */
static void test_v9_session_fields(void)
{
    printf("--- 16. v9 session fields: round-trip, refusal, v8, saturation ---\n");

    struct pgwt_summary_accum *acc = malloc(sizeof(*acc));
    struct pgwt_summary_accum *back = malloc(sizeof(*back));
    uint8_t *v9 = malloc(PGWT_SUMMARY_SERIALIZE_MAX);
    uint8_t *v8 = malloc(PGWT_SUMMARY_SERIALIZE_MAX);
    CHECK(acc && back && v9 && v8, "16: alloc accumulators and buffers");
    if (!acc || !back || !v9 || !v8) {
        free(acc); free(back); free(v9); free(v8); return;
    }
    memset(acc, 0, sizeof(*acc));
    acc->second_wall_ns = 1700000000ULL * ONE_SEC;
    acc->sessions[0].pid = 301;
    acc->sessions[0].db_time_ns = 15 * MS;
    acc->sessions[0].cpu_ns = 10 * MS;
    acc->sessions[0].top_wait_id = EV_LOCK;
    acc->sessions[0].top_wait_ns = 5 * MS;
    acc->sessions[0].cpu_offcpu_ns = 6 * MS;
    acc->sessions[0].cpu_measured_records = 7;
    acc->num_sessions = 1;
    /* Non-zero trailer canaries: a v8 reader that wrongly consumes a v9
     * session extension would copy these into the new fields, not zeros. */
    acc->idle_ns = 23 * MS;
    acc->events_overflow = 11;

    /* 16a. Distinct non-zero values prevent a wrong-offset copy from
     * passing as a successful round trip of both fields. */
    CHECK(acc->sessions[0].cpu_offcpu_ns != 0 &&
          acc->sessions[0].cpu_measured_records != 0 &&
          acc->sessions[0].cpu_offcpu_ns != acc->sessions[0].cpu_measured_records &&
          acc->sessions[0].cpu_offcpu_ns != acc->sessions[0].cpu_ns &&
          acc->sessions[0].cpu_measured_records != acc->sessions[0].cpu_ns,
          "16a non-vacuity: both new fields are non-zero and distinct "
          "from each other and cpu_ns");
    size_t size = pgwt_summary_serialize(acc, v9, PGWT_SUMMARY_SERIALIZE_MAX);
    const size_t prefix = (size_t)PGWT_NUM_CLASSES * 8;
    CHECK(size == prefix + 44 + 12 + 40,
          "16a: one v9 session and both trailers serialize (%zu bytes)", size);
    if (size != prefix + 44 + 12 + 40) {
        free(acc); free(back); free(v9); free(v8); return;
    }
    memset(back, 0, sizeof(*back));
    back->second_wall_ns = acc->second_wall_ns;
    back->num_sessions = 1;
    int rc = pgwt_summary_deserialize(v9, size, back, 9);
    CHECK(rc == 0, "16a: v9 session deserializes (rc=%d)", rc);
    const struct pgwt_summary_session *ss = NULL;
    for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++)
        if (back->sessions[i].pid == 301) { ss = &back->sessions[i]; break; }
    CHECK(ss != NULL, "16a: the decoded session exists");
    if (ss) {
        CHECK(ss->cpu_offcpu_ns == acc->sessions[0].cpu_offcpu_ns,
              "16a: cpu_offcpu_ns round-trips EXACTLY (%llu)",
              (unsigned long long)ss->cpu_offcpu_ns);
        CHECK(ss->cpu_measured_records == acc->sessions[0].cpu_measured_records,
              "16a: cpu_measured_records round-trips EXACTLY (%u)",
              ss->cpu_measured_records);
    }

    /* 16b. End INSIDE the 44-byte session entry, not inside a trailer.
     * Zero-filling this short record would invent an unmeasured window.
     *
     * WHAT THIS LEG DOES NOT PROVE, stated because the opposite is the
     * natural reading: it does NOT isolate the per-entry bounds test in
     * pgwt_summary_deserialize's session loop. That test cannot change this
     * function's RETURN VALUE for any truncation, and that was measured, not
     * assumed -- with the session test replaced by a bare `p > end` the
     * parser over-reads the entry and is then caught by the v4 trailer's own
     * `p + 12 > end` check, so rc is -1 either way and this CHECK stays
     * green. The session test is a MEMORY-SAFETY guard (it stops a 4-byte
     * read past the buffer), and demonstrating it needs a sanitizer build,
     * which tests/Makefile cannot currently produce for this binary. What
     * this leg does prove is the end-to-end contract that matters to a
     * reader: a truncated v9 record is REFUSED rather than decoding to an
     * unmeasured window. */
    memset(back, 0, sizeof(*back));
    back->second_wall_ns = acc->second_wall_ns;
    back->num_sessions = 1;
    rc = pgwt_summary_deserialize(v9, prefix + 40, back, 9);
    CHECK(rc != 0, "16b: a truncated v9 session is REFUSED (rc=%d)", rc);

    /* 16c. Reconstruct the actual v8 payload: keep the 32-byte session,
     * drop its 12 v9 bytes, and retain both record trailers. */
    memcpy(v8, v9, prefix + 32);
    memcpy(v8 + prefix + 32, v9 + prefix + 44, size - prefix - 44);
    memset(back, 0, sizeof(*back));
    back->second_wall_ns = acc->second_wall_ns;
    back->num_sessions = 1;
    rc = pgwt_summary_deserialize(v8, size - 12, back, 8);
    CHECK(rc == 0, "16c: the actual v8 payload parses as v8 (rc=%d)", rc);
    ss = NULL;
    for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++)
        if (back->sessions[i].pid == 301) { ss = &back->sessions[i]; break; }
    CHECK(ss != NULL, "16c: the v8 session exists");
    if (ss) {
        CHECK(ss->db_time_ns == acc->sessions[0].db_time_ns,
              "16c: v8 DB Time came back (%llu)",
              (unsigned long long)ss->db_time_ns);
        CHECK(ss->cpu_ns == acc->sessions[0].cpu_ns,
              "16c: v8 CPU wall came back (%llu)",
              (unsigned long long)ss->cpu_ns);
        /* These zeros are meaningful beside 16a's distinct non-zero
         * round trip: reading v9 bytes at v8 offsets must not invent CPU. */
        CHECK(ss->cpu_offcpu_ns == 0,
              "16c: v8 retains zero off-CPU, refusing to split");
        CHECK(ss->cpu_measured_records == 0,
              "16c: v8 retains zero observations, refusing a measurement");
    }

    /* 16d. Reach the saturation arm through the public push path: seed a
     * real session, set its counter to UINT32_MAX, then add another measured
     * gap in the SAME second. The time increments witness that it ran. */
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    CHECK(w != NULL, "16d: alloc writer");
    if (w) {
        int init_rc = pgwt_summary_writer_init(w, fresh_dir("v9_saturation"),
                                                24, NULL);
        CHECK(init_rc == 0, "16d: writer initializes (rc=%d)", init_rc);
        if (init_rc == 0) {
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            e.timestamp_ns = mono_origin() + 500 * MS;
            e.pid = 301;
            e.old_event = 0;
            e.new_event = EV_LOCK;
            e.duration_ns = 10 * MS;
            e.cpu_ns = 4 * MS;
            int push_rc = pgwt_summary_push_event(w, &e, 0, e.old_event);
            CHECK(push_rc == 0, "16d: first measured gap accepted (%d)", push_rc);
            struct pgwt_summary_session *live = NULL;
            for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++)
                if (w->accum.sessions[i].pid == e.pid) {
                    live = &w->accum.sessions[i]; break;
                }
            CHECK(live != NULL, "16d: the real writer created the session");
            if (live) {
                CHECK(live->cpu_measured_records == 1,
                      "16d: first gap counted one measurement (%u)",
                      live->cpu_measured_records);
                CHECK(live->cpu_offcpu_ns == 6 * MS,
                      "16d: first gap contributed 6 ms off-CPU (%llu)",
                      (unsigned long long)live->cpu_offcpu_ns);
                live->cpu_measured_records = UINT32_MAX;
                e.timestamp_ns += 20 * MS;
                push_rc = pgwt_summary_push_event(w, &e, 0, e.old_event);
                CHECK(push_rc == 0, "16d: next measured gap accepted (%d)", push_rc);
                CHECK(live->cpu_ns == 20 * MS,
                      "16d: both gaps contributed wall time (%llu)",
                      (unsigned long long)live->cpu_ns);
                CHECK(live->cpu_offcpu_ns == 12 * MS,
                      "16d: the next measurement contributed its remainder (%llu)",
                      (unsigned long long)live->cpu_offcpu_ns);
                CHECK(live->cpu_measured_records == UINT32_MAX,
                      "16d: the real writer SATURATES instead of wrapping (%u)",
                      live->cpu_measured_records);

                size = pgwt_summary_serialize(&w->accum, v9,
                                              PGWT_SUMMARY_SERIALIZE_MAX);
                CHECK(size > 0, "16d: serialize the saturated session (%zu)", size);
                memset(back, 0, sizeof(*back));
                back->second_wall_ns = w->accum.second_wall_ns;
                back->num_events = w->accum.num_events;
                back->num_sessions = w->accum.num_sessions;
                back->num_queries = w->accum.num_queries;
                rc = pgwt_summary_deserialize(v9, size, back, 9);
                CHECK(rc == 0, "16d: saturated v9 session parses (%d)", rc);
                ss = NULL;
                for (int i = 0; i < SUMMARY_MAX_SESSIONS; i++)
                    if (back->sessions[i].pid == e.pid) {
                        ss = &back->sessions[i]; break;
                    }
                CHECK(ss != NULL, "16d: the saturated session exists on readback");
                if (ss)
                    CHECK(ss->cpu_measured_records == UINT32_MAX,
                          "16d: UINT32_MAX survives the format as UINT32_MAX (%u)",
                          ss->cpu_measured_records);
            }
            pgwt_summary_close(w);
            pgwt_summary_destroy(w);
        }
        free(w);
    }
    free(acc); free(back); free(v9); free(v8);
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
    test_bounded_tables();
    test_top_queries_filtered_excludes_idle();
    test_idle_children_sum_exactly();
    test_old_layout_is_incompatible();
    test_zero_parent_excess_is_detected();
    test_refused_serialize_is_counted();
    test_failed_rotation_discards_the_second();
    test_bypass_suite();
    test_routing_refuses_an_unexamined_window();
    test_archived_file_is_timezone_independent();
    test_unplaceable_archive_is_refused();
    test_sessions_overflow_is_reported();
    test_measured_cpu_inside_visible_idle();
    test_v9_session_fields();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
    if (system(cmd) != 0) { /* ignore */ }

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
