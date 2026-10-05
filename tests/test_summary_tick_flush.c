/* test_summary_tick_flush.c — one logical second contributes exactly once.
 *
 * THE DEFECT (#277). pgwt_summary_flush ran on EVERY daemon tick, ungated.
 * flush_accum serialised the IN-PROGRESS second, wrote the block and cleared
 * accum_active — but left every counter in place. The next event in that same
 * second re-activated the accumulator (a path that only sets the timestamps),
 * so counting continued on top of counts already on disk, and at the real
 * second boundary the same second was written again as a superset.
 * pgwt_visit_summaries sums every record it finds, so each split second was
 * counted twice or more. Measured on a gate box: a 121 s unfiltered window
 * read 1.469x what the identical pid-filtered raw window read, and up to
 * 2.157x at -i 1, where almost every second is split by a tick.
 *
 * WHAT THIS PINS
 *   1. the tick flush leaves a second that is still in progress alone, and
 *      writes it once it is complete (section 1);
 *   2. completeness is decided on the CLOCK, not on event arrival — a second
 *      that simply stops receiving events is still written on the next tick
 *      (section 2). This is the "stalls instead of failing" direction;
 *   3. a second already on disk can never be reopened: an event that arrives
 *      late folds into the next open second, counted exactly once, and the
 *      fold is counted rather than silent (section 3);
 *   4. the unconditional pgwt_summary_flush, which rotation, close and the
 *      offline generators still use, cannot double-count either (section 4).
 *
 * WHY SECTION 5 EXISTS (false negatives, not true positives). "Reader-side
 * sum equals what we pushed" is satisfied by 0 == 0. Section 5 walks every
 * way this check can be made unable to see: no summary file at all, a writer
 * that never wrote, an unparseable file, a missing directory, and a directory
 * that contains the SAME second twice (the exact shape of the bug) — each
 * must be reported as a refusal or a red, never as agreement.
 *
 * TIMING. Nothing here reads a clock to make a decision:
 * pgwt_summary_flush_completed takes `now_mono_ns` as a parameter, so every
 * tick in this file is an exact nanosecond literal. The one real clock read
 * (the writer's wall/mono offset, captured when it opens its file) only
 * affects the wall timestamps stamped on records; no assertion depends on
 * their value, only on two different seconds being different. No sleeps, no
 * retries, no tolerances.
 */
#include "summary_writer.h"
#include "summary_reader.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, fmt, ...) do {                                    \
    tests_run++;                                                      \
    if (cond) { printf("  ok: " fmt "\n", ##__VA_ARGS__); }           \
    else { tests_failed++;                                            \
           printf("  FAIL(%d): " fmt "\n", __LINE__, ##__VA_ARGS__); }\
} while (0)

#define ONE_SEC 1000000000ULL

/* Per-process scratch dir: a fixed /tmp path collides across runs on a
 * shared box (#125). */
static char g_base[300];

static const char *subdir(const char *name)
{
    static char buf[600];
    snprintf(buf, sizeof(buf), "%s/%s", g_base, name);
    return buf;
}

static const char *fresh_dir(const char *name)
{
    const char *d = subdir(name);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", d);
    if (system(cmd) != 0) { /* ignore */ }
    mkdir(d, 0755);
    return d;
}

static struct pgwt_trace_event mk_event(uint64_t ts, uint32_t pid,
                                        uint32_t old_ev, uint64_t dur)
{
    struct pgwt_trace_event ev = {0};
    ev.timestamp_ns = ts;
    ev.pid = pid;
    ev.old_event = old_ev;
    ev.new_event = 0;
    ev.duration_ns = dur;
    ev.query_id = 0;
    return ev;
}

/* The one event type used throughout: IO (class 0x0A), never a marker, never
 * PGWT_EVENT_EXIT, so accum_event always counts it. */
#define EV_ID   0x0A000001u
#define EV_DUR  1000000ULL            /* 1 ms */

/* ── reader side ──────────────────────────────────────────────────────── */

#define MAX_REC 64

struct sum_ctx {
    int      records;
    uint64_t events;              /* Σ over all per-event slots */
    uint64_t total_ns;            /* Σ class_ns[] */
    uint64_t sec_wall[MAX_REC];   /* one per record, in visit order */
    uint64_t rec_events[MAX_REC];
    int      duplicate_seconds;   /* records whose second was already seen */
};

static int sum_visitor(const struct pgwt_summary_accum *rec, void *vctx)
{
    struct sum_ctx *c = vctx;
    uint64_t n = 0;
    /* Open-addressed table: walk every slot, not the first num_events. */
    for (int i = 0; i < SUMMARY_MAX_EVENTS; i++)
        if (rec->events[i].event_id != 0)
            n += rec->events[i].count;
    for (int i = 0; i < PGWT_NUM_CLASSES; i++)
        c->total_ns += rec->class_ns[i];

    for (int i = 0; i < c->records && i < MAX_REC; i++)
        if (c->sec_wall[i] == rec->second_wall_ns)
            c->duplicate_seconds++;
    if (c->records < MAX_REC) {
        c->sec_wall[c->records]   = rec->second_wall_ns;
        c->rec_events[c->records] = n;
    }
    c->records++;
    c->events += n;
    return 0;
}

/* Returns pgwt_visit_summaries' own return value: < 0 or 0 means the read
 * saw nothing, which callers must treat as a refusal, never as "sums
 * agree". */
static int read_summaries(const char *dir, struct sum_ctx *c)
{
    memset(c, 0, sizeof(*c));
    return pgwt_visit_summaries(dir, 0, 0, sum_visitor, c);
}

/* The conservation gate. `want_events` and `want_records` are literals the
 * caller counted itself while pushing — never a value read back out of the
 * writer or the file, which is how a conservation check ends up comparing a
 * sum against itself. A zero expectation is rejected outright: it would make
 * every later assertion satisfiable by an empty trace. */
static void check_conserved(const char *what, const char *dir,
                            uint64_t want_events, int want_records)
{
    struct sum_ctx c;
    CHECK(want_events > 0 && want_records > 0,
          "%s: expectation is non-empty (%llu events, %d records)", what,
          (unsigned long long)want_events, want_records);
    int rc = read_summaries(dir, &c);
    CHECK(rc == want_records,
          "%s: reader visited %d records (want %d)", what, rc, want_records);
    CHECK(c.records == want_records,
          "%s: visitor saw %d records (want %d)", what, c.records,
          want_records);
    CHECK(c.duplicate_seconds == 0,
          "%s: no second appears in two records (%d duplicates)", what,
          c.duplicate_seconds);
    CHECK(c.events == want_events,
          "%s: reader-side event count %llu (want %llu)", what,
          (unsigned long long)c.events, (unsigned long long)want_events);
    CHECK(c.total_ns == want_events * EV_DUR,
          "%s: reader-side time %llu ns (want %llu)", what,
          (unsigned long long)c.total_ns,
          (unsigned long long)(want_events * EV_DUR));
}

/* A monotonic base to hang synthetic seconds off, so the wall timestamps the
 * writer stamps on records stay sane. Nothing decides anything from it. */
static uint64_t mono_base_second(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t mono = (uint64_t)ts.tv_sec * ONE_SEC + (uint64_t)ts.tv_nsec;
    return (mono / ONE_SEC) * ONE_SEC;
}

static struct pgwt_summary_writer *new_writer(const char *dir)
{
    struct pgwt_summary_writer *w = calloc(1, sizeof(*w));
    if (!w) { printf("  FAIL: calloc\n"); exit(1); }
    if (pgwt_summary_writer_init(w, dir, 24, NULL) != 0) {
        printf("  FAIL: writer init in %s\n", dir);
        exit(1);
    }
    return w;
}

static void push_n(struct pgwt_summary_writer *w, uint64_t first_ts,
                   uint64_t step_ns, int n)
{
    for (int i = 0; i < n; i++) {
        struct pgwt_trace_event ev =
            mk_event(first_ts + (uint64_t)i * step_ns, 1000, EV_ID, EV_DUR);
        pgwt_summary_push_event(w, &ev);
    }
}

static void done(struct pgwt_summary_writer *w)
{
    pgwt_summary_destroy(w);
    free(w);
}

/* ── 1. the defect: a tick must not write a second still in progress ──── */

static void test_tick_does_not_split_a_second(void)
{
    printf("--- 1. tick flush vs. the in-progress second (#277) ---\n");
    const char *dir = fresh_dir("tick");
    const uint64_t B = mono_base_second();
    struct pgwt_summary_writer *w = new_writer(dir);

    /* 40 events in the first half of second B. */
    push_n(w, B, 10000000ULL, 40);                 /* B+0 .. B+390 ms */

    /* A tick lands mid-second. On master this wrote second B right here. */
    pgwt_summary_flush_completed(w, B + 500000000ULL);
    CHECK(w->total_records_written == 0,
          "tick at B+0.5s writes nothing (second in progress), got %llu",
          (unsigned long long)w->total_records_written);

    /* A tick just past the end but inside the in-flight lag: still nothing. */
    pgwt_summary_flush_completed(w, B + ONE_SEC + PGWT_SUMMARY_FLUSH_LAG_NS - 1);
    CHECK(w->total_records_written == 0,
          "tick at the last ns before the lag expires writes nothing, got %llu",
          (unsigned long long)w->total_records_written);

    /* 20 more events, still inside second B — these are the ones master
     * counted twice (once in the mid-second record, again in the superset
     * written at the real boundary). */
    push_n(w, B + 500000000ULL, 20000000ULL, 20);  /* B+500 .. B+880 ms */

    /* Second B is complete: one tick writes it, once. */
    pgwt_summary_flush_completed(w, B + ONE_SEC + PGWT_SUMMARY_FLUSH_LAG_NS);
    CHECK(w->total_records_written == 1,
          "tick after B ends writes exactly one record, got %llu",
          (unsigned long long)w->total_records_written);

    /* 30 events in second B+1, then a tick past its end. */
    push_n(w, B + ONE_SEC, 10000000ULL, 30);
    pgwt_summary_flush_completed(w, B + 2 * ONE_SEC + PGWT_SUMMARY_FLUSH_LAG_NS);
    CHECK(w->total_records_written == 2,
          "second record is second B+1, got %llu records",
          (unsigned long long)w->total_records_written);
    CHECK(w->late_events_folded_total == 0,
          "no event was late, so none was folded (%llu)",
          (unsigned long long)w->late_events_folded_total);

    /* 90 events pushed, 90 events readable, each second once.
     * Master writes 40 + 60 + 90 = 190 here, across 3 records, with second B
     * appearing twice — both the count and the duplicate check go red. */
    check_conserved("tick-split second", dir, 90, 2);

    struct sum_ctx c;
    read_summaries(dir, &c);
    CHECK(c.records == 2 && c.rec_events[0] == 60 && c.rec_events[1] == 30,
          "per-second split is 60 then 30 (got %llu then %llu)",
          (unsigned long long)c.rec_events[0],
          (unsigned long long)c.rec_events[1]);
    CHECK(c.records == 2 && c.sec_wall[0] != c.sec_wall[1],
          "the two records carry two different seconds");

    done(w);
}

/* ── 2. the gate must not stall: a quiet second is still written ──────── */

static void test_completed_second_is_written_without_further_events(void)
{
    printf("--- 2. a completed second is written on the next tick ---\n");
    const char *dir = fresh_dir("quiet");
    const uint64_t B = mono_base_second();
    struct pgwt_summary_writer *w = new_writer(dir);

    push_n(w, B, 50000000ULL, 10);     /* 10 events, then silence forever */

    /* One nanosecond before the bound: nothing. This is what distinguishes a
     * real gate from `return 0;` — a stub passes the line below but fails
     * this one by never writing at all. */
    pgwt_summary_flush_completed(w, B + ONE_SEC + PGWT_SUMMARY_FLUSH_LAG_NS - 1);
    CHECK(w->total_records_written == 0,
          "not yet complete at end+lag-1ns, got %llu",
          (unsigned long long)w->total_records_written);

    /* Exactly at the bound: written, with no further event to trigger it. */
    pgwt_summary_flush_completed(w, B + ONE_SEC + PGWT_SUMMARY_FLUSH_LAG_NS);
    CHECK(w->total_records_written == 1,
          "written at end+lag with no further events, got %llu",
          (unsigned long long)w->total_records_written);

    /* And a repeated tick must not write it again. */
    pgwt_summary_flush_completed(w, B + 10 * ONE_SEC);
    CHECK(w->total_records_written == 1,
          "later ticks do not rewrite an already-written second, got %llu",
          (unsigned long long)w->total_records_written);

    check_conserved("quiet second", dir, 10, 1);
    done(w);
}

/* ── 3. a written second can never be reopened ────────────────────────── */

static void test_late_event_folds_forward(void)
{
    printf("--- 3. late event for a written second folds forward ---\n");
    const char *dir = fresh_dir("late");
    const uint64_t B = mono_base_second();
    struct pgwt_summary_writer *w = new_writer(dir);

    push_n(w, B, 50000000ULL, 10);
    pgwt_summary_flush_completed(w, B + ONE_SEC + PGWT_SUMMARY_FLUSH_LAG_NS);
    CHECK(w->total_records_written == 1, "second B written once");

    /* Five events for second B arrive after B was written (the event ring is
     * drained after the timer handler in the same main-loop pass). */
    push_n(w, B + 900000000ULL, 10000000ULL, 5);
    CHECK(w->late_events_folded_total == 5,
          "5 late events counted as folded, got %llu",
          (unsigned long long)w->late_events_folded_total);

    pgwt_summary_flush_completed(w, B + 2 * ONE_SEC + PGWT_SUMMARY_FLUSH_LAG_NS);
    CHECK(w->total_records_written == 2,
          "the folded events are written as second B+1, got %llu records",
          (unsigned long long)w->total_records_written);

    /* 15 pushed, 15 readable, two distinct seconds. Without the fold the
     * writer would open second B a second time and the reader would sum B
     * twice (duplicate_seconds goes to 1 and the per-record split changes). */
    check_conserved("late arrival", dir, 15, 2);
    struct sum_ctx c;
    read_summaries(dir, &c);
    CHECK(c.rec_events[0] == 10 && c.rec_events[1] == 5,
          "late events land in the NEXT second (got %llu then %llu)",
          (unsigned long long)c.rec_events[0],
          (unsigned long long)c.rec_events[1]);
    done(w);
}

/* ── 4. the force-flush path (rotation / close / generators) ──────────── */

static void test_force_flush_cannot_double_count(void)
{
    printf("--- 4. unconditional pgwt_summary_flush ---\n");
    const char *dir = fresh_dir("force");
    const uint64_t B = mono_base_second();
    struct pgwt_summary_writer *w = new_writer(dir);

    push_n(w, B, 10000000ULL, 10);
    /* Unlike the tick, this writes the in-progress second immediately — the
     * generators and pgwt_summary_close depend on that. */
    pgwt_summary_flush(w);
    CHECK(w->total_records_written == 1,
          "force flush writes the in-progress second, got %llu",
          (unsigned long long)w->total_records_written);

    /* The rest of the same second must NOT resume the written record. */
    push_n(w, B + 500000000ULL, 10000000ULL, 5);
    CHECK(w->late_events_folded_total == 5,
          "the remainder of the second folds forward, got %llu",
          (unsigned long long)w->late_events_folded_total);
    pgwt_summary_flush(w);
    CHECK(w->total_records_written == 2, "second record written");

    /* A force flush with nothing accumulated writes nothing. */
    pgwt_summary_flush(w);
    CHECK(w->total_records_written == 2,
          "force flush on an empty accumulator is a no-op, got %llu",
          (unsigned long long)w->total_records_written);

    check_conserved("force flush", dir, 15, 2);
    done(w);
}

/* ── 5. bypass suite: every way this gate could fail to see ───────────── */

static void test_bypass_suite(void)
{
    printf("--- 5. bypass suite (false negatives) ---\n");

    /* 5a. No summary file at all. The conservation sum would be 0 == 0; the
     *     reader must report nothing visited so the caller refuses. */
    {
        const char *dir = fresh_dir("empty");
        struct sum_ctx c;
        int rc = read_summaries(dir, &c);
        CHECK(rc <= 0 && c.records == 0 && c.events == 0,
              "5a empty dir: reader refuses (rc=%d, %d records)", rc,
              c.records);
    }

    /* 5b. A writer that never wrote (disabled before any push). Same 0 == 0
     *     trap, reached through the product rather than the filesystem. */
    {
        const char *dir = fresh_dir("disabled");
        const uint64_t B = mono_base_second();
        struct pgwt_summary_writer *w = new_writer(dir);
        w->enabled = false;
        push_n(w, B, 10000000ULL, 10);
        pgwt_summary_flush_completed(w, B + 5 * ONE_SEC);
        CHECK(w->total_records_written == 0, "5b disabled writer wrote nothing");
        struct sum_ctx c;
        int rc = read_summaries(dir, &c);
        CHECK(rc <= 0 && c.events == 0,
              "5b disabled writer: reader refuses (rc=%d)", rc);
        /* And the gate that matters: the record-count assertion in
         * check_conserved compares against a literal, so it would go red
         * here rather than agree with an empty read. */
        CHECK(c.records != 1, "5b an empty read is not 1 record");
        done(w);
    }

    /* 5c. The SAME second present twice — the exact shape of #277, built by
     *     hand so the detector is proved able to see it. One closed archive
     *     is copied to a second archive name, so the two records carry
     *     byte-identical seconds (two live writers would each capture their
     *     own wall/mono offset, a few hundred ns apart, and the duplicate
     *     would not be exactly equal). A check that only asked "is the sum
     *     positive", or that looked at one file, would pass here. */
    {
        const char *dir = fresh_dir("dup");
        const uint64_t B = mono_base_second();
        struct pgwt_summary_writer *w = new_writer(dir);
        push_n(w, B, 10000000ULL, 10);
        pgwt_summary_flush_completed(w, B + ONE_SEC + PGWT_SUMMARY_FLUSH_LAG_NS);
        pgwt_summary_close(w);
        done(w);

        char cmd[900];
        /* pgwt_summary_close writes the footer but leaves the file named
         * current.summary (the next daemon start archives it). Copying it
         * under an archive name gives two files whose single record is
         * byte-identical. */
        snprintf(cmd, sizeof(cmd),
                 "cp %s/current.summary %s/2020-01-01_05.summary.lz4",
                 dir, dir);
        CHECK(system(cmd) == 0, "5c archive duplicated on disk");

        struct sum_ctx c;
        int rc = read_summaries(dir, &c);
        CHECK(rc == 2 && c.records == 2,
              "5c two files, two records visited (rc=%d)", rc);
        CHECK(c.duplicate_seconds == 1,
              "5c the duplicated second IS detected (%d duplicates)",
              c.duplicate_seconds);
        CHECK(c.events == 20,
              "5c the duplicate inflates the sum to 20 (got %llu)",
              (unsigned long long)c.events);
    }

    /* 5d. An unparseable summary file. The reader must skip/refuse, never
     *     hand back a partially-decoded record that passes as agreement. */
    {
        const char *dir = fresh_dir("garbage");
        char path[700];
        snprintf(path, sizeof(path), "%s/current.summary", dir);
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL, "5d garbage file created");
        if (f) {
            unsigned char junk[128];
            memset(junk, 0xA5, sizeof(junk));
            size_t nw = fwrite(junk, 1, sizeof(junk), f);
            fclose(f);
            CHECK(nw == sizeof(junk), "5d garbage written");
        }
        struct sum_ctx c;
        int rc = read_summaries(dir, &c);
        CHECK(rc <= 0 && c.events == 0,
              "5d bad magic: reader refuses (rc=%d, %llu events)", rc,
              (unsigned long long)c.events);
    }

    /* 5e. The directory does not exist at all. */
    {
        const char *dir = subdir("no-such-dir");
        char cmd[700];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        if (system(cmd) != 0) { /* ignore */ }
        struct sum_ctx c;
        int rc = read_summaries(dir, &c);
        CHECK(rc <= 0 && c.records == 0,
              "5e missing dir: reader refuses (rc=%d)", rc);
    }

    /* 5f. A second with no events never produces a record, so "records ==
     *     seconds elapsed" would be wrong as a gate; the gate is "records ==
     *     seconds that had events". Ticking across a silent gap must add
     *     nothing. */
    {
        const char *dir = fresh_dir("gap");
        const uint64_t B = mono_base_second();
        struct pgwt_summary_writer *w = new_writer(dir);
        push_n(w, B, 10000000ULL, 10);
        pgwt_summary_flush_completed(w, B + ONE_SEC + PGWT_SUMMARY_FLUSH_LAG_NS);
        for (int t = 2; t <= 6; t++)
            pgwt_summary_flush_completed(w, B + (uint64_t)t * ONE_SEC);
        CHECK(w->total_records_written == 1,
              "5f five silent ticks add no records (got %llu)",
              (unsigned long long)w->total_records_written);
        /* The next real event is 5 s later and must open its own second. */
        push_n(w, B + 6 * ONE_SEC, 10000000ULL, 7);
        CHECK(w->late_events_folded_total == 0,
              "5f a forward-in-time event is not treated as late (%llu)",
              (unsigned long long)w->late_events_folded_total);
        pgwt_summary_flush_completed(w, B + 8 * ONE_SEC);
        check_conserved("5f gap", dir, 17, 2);
        done(w);
    }
}

int main(void)
{
    snprintf(g_base, sizeof(g_base), "/tmp/pgwt_tickflush_XXXXXX");
    if (!mkdtemp(g_base)) {
        perror("mkdtemp");
        return 1;
    }
    printf("=== test_summary_tick_flush (#277) ===\n");
    printf("scratch: %s\n", g_base);

    test_tick_does_not_split_a_second();
    test_completed_second_is_written_without_further_events();
    test_late_event_folds_forward();
    test_force_flush_cannot_double_count();
    test_bypass_suite();

    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_base);
    if (system(cmd) != 0) { /* ignore */ }

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
