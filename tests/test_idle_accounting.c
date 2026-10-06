/* test_idle_accounting.c — timer-sleep waits leave DB Time, and the Idle row
 * says WHERE the time went (2026-10-06).
 *
 * THE RULE (owner, verbatim): "there is DB time when database doing smth - not
 * just waiting for timer" / "if database is consuming any resources (CPU, IO
 * and so on) - it is not idle and must go to DB time". Six Timeout events are
 * pure pacing sleeps and leave DB Time and AAS; Timeout:PgSleep,
 * Timeout:SpinDelay, Timeout:RegisterSyncRequest and Timeout:VacuumTruncate
 * stay in it. src/idle_rule.c is the one list; tests/test_wait_event.c pins the
 * classification per version. THIS file pins what the RAW compute layer does
 * with it.
 *
 * WHY A PREDICATE TEST IS NOT ENOUGH, which is what each section exists for:
 *
 *  1. DB Time, the class rows and the Timeout class total. A pacing sleep must
 *     leave DB Time while PgSleep and SpinDelay stay, so the Timeout CLASS row
 *     is no longer "all Timeout time" — it is a strict subset, and an
 *     implementation that excluded the whole class would pass a test that only
 *     checked DB Time.
 *  2. VISIBILITY. Excluding from load must not delete the rows: the pacing
 *     events still appear as indent-2 sub-events and in Top Events with the
 *     idle %DB sentinel. The ClientRead precedent — pgwt_is_idle_event yes,
 *     pgwt_is_hidden_event no.
 *  3. THE IDLE ROW AND ITS NAMED CHILDREN. Before this change the time model
 *     emitted no Idle row at all, so idle_time_ms was a number with nothing
 *     behind it. Making 807 s of CheckpointWriteDelay idle without naming it
 *     would have moved an unexplained figure onto the Overview, which is the
 *     opposite of the point. Also pins the INDENTS, which are load-bearing for
 *     two gates in tests/demo_rehearsal_lib.py (see section 3).
 *  4. CONSERVATION, by COMPONENT and not only by identity. The identity
 *     CPU* + Off-CPU* + sum(waits) = DB Time can close while both sides omit
 *     the same time — Off-CPU* is defined as the residual, so anything a bug
 *     fails to attribute flows into it and the identity still holds. So every
 *     component is asserted against a number computed from the fixture.
 *  5. CPU ON and ADJACENT TO a pacing wait. src/compute.c skips the whole idle
 *     interval INCLUDING its measured cpu_ns. That is the ClientRead precedent
 *     and it generalises, but it means CPU measured inside a pacing span is
 *     dropped — and CPU measurement does spill across interval boundaries
 *     (se.sum_exec_runtime is only current at a tick or a context switch). This
 *     section asserts the behaviour and asserts that pacing and ClientRead are
 *     treated IDENTICALLY, which is the claim "we followed the precedent".
 *  6. FALSE NEGATIVES. Every assertion above is satisfiable by an empty or
 *     Timeout-free fixture: 0 ms excluded == 0 ms expected. Section 6 walks the
 *     ways this file could stop being able to see — nothing in the window, no
 *     pacing event present at all, the event present but with zero duration, a
 *     window that clips everything away, and the structural trap (an indent-1
 *     Idle row) that would silently break a gate in another file.
 *
 * TIMING. Nothing here reads a clock. Every timestamp and duration is a
 * literal; windows are passed explicitly. No sleeps, no tolerances except the
 * floating-point epsilon on sums of doubles.
 */
#include "compute.h"
#include "wait_event.h"
#include "idle_rule.h"
#include "pg_wait_tracer.h"
#include "summary_reader.h"   /* pgwt_summary_visitor, for the stub below */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, fmt, ...) do {                                     \
    tests_run++;                                                       \
    if (cond) { }                                                      \
    else { tests_failed++;                                             \
           printf("  FAIL(line %d): " fmt "\n", __LINE__, ##__VA_ARGS__); } \
} while (0)

#define EPS 1e-6
#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < EPS)

/* compute.c's only foreign symbol in this link set. Never reached: every test
 * here calls the RAW entry points. Aborting rather than returning makes a
 * future change that routed a raw path through summaries a loud failure
 * instead of a quietly different answer. */
int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns;
    (void)visitor; (void)ctx;
    fprintf(stderr, "BUG: test_idle_accounting reached the summary path\n");
    abort();
}

/* ── fixture helpers ───────────────────────────────────────────────────── */

#define MS 1000000ULL

/* PG18 Timeout ids (asserted by name in tests/test_wait_event.c). */
#define EV_CHECKPOINT_DELAY WEI(PG_WAIT_TIMEOUT, 1)   /* pacing */
#define EV_PGSLEEP          WEI(PG_WAIT_TIMEOUT, 2)   /* DB Time */
#define EV_SPINDELAY        WEI(PG_WAIT_TIMEOUT, 6)   /* DB Time */
#define EV_VACUUM_DELAY     WEI(PG_WAIT_TIMEOUT, 7)   /* pacing */
#define EV_CLIENTREAD       WEI(PG_WAIT_CLIENT, 0)    /* idle, visible */
#define EV_CLIENTWRITE      WEI(PG_WAIT_CLIENT, 1)    /* DB Time */
#define EV_ACTIVITY         WEI(PG_WAIT_ACTIVITY, 4)  /* idle AND hidden */
#define EV_LOCK             WEI(PG_WAIT_LOCK, 0)
#define EV_IO               WEI(PG_WAIT_IO, 21)

static struct pgwt_trace_event mk(uint64_t ts, uint32_t pid, uint32_t old_ev,
                                  uint64_t dur_ms, uint64_t cpu_ns)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;
    e.pid          = pid;
    e.old_event    = old_ev;
    e.new_event    = EV_IO;          /* never EXIT, never a marker */
    e.duration_ns  = dur_ms * MS;
    e.query_id     = 0;
    e.cpu_ns       = cpu_ns;
    return e;
}

static const struct pgwt_tm_row *row_by_name(const struct pgwt_tm_result *tm,
                                             const char *name)
{
    for (int i = 0; i < tm->num_rows; i++)
        if (strcmp(tm->rows[i].name, name) == 0)
            return &tm->rows[i];
    return NULL;
}

static const struct pgwt_tm_row *row_at(const struct pgwt_tm_result *tm,
                                        const char *name, int indent)
{
    for (int i = 0; i < tm->num_rows; i++)
        if (strcmp(tm->rows[i].name, name) == 0 && tm->rows[i].indent == indent)
            return &tm->rows[i];
    return NULL;
}

static double sum_indent(const struct pgwt_tm_result *tm, int indent)
{
    double t = 0;
    for (int i = 0; i < tm->num_rows; i++)
        if (tm->rows[i].indent == indent)
            t += tm->rows[i].time_ms;
    return t;
}

static int count_indent(const struct pgwt_tm_result *tm, int indent)
{
    int n = 0;
    for (int i = 0; i < tm->num_rows; i++)
        if (tm->rows[i].indent == indent) n++;
    return n;
}

static const struct pgwt_event_row *ev_row(const struct pgwt_events_result *r,
                                           uint32_t eid)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].event_id == eid) return &r->rows[i];
    return NULL;
}

/* ── THE FIXTURE ───────────────────────────────────────────────────────────
 * Deliberately built so that every wrong answer produces a DIFFERENT number,
 * never a coincidence:
 *
 *   load        Lock            300 ms
 *               IO              200 ms
 *               Client:ClientWrite 50 ms
 *               Timeout:PgSleep    400 ms   <- Timeout, but DB Time
 *               Timeout:SpinDelay  100 ms   <- Timeout, but DB Time
 *               CPU (we==0)        150 ms wall, 120 ms measured CPU
 *   idle        Timeout:CheckpointWriteDelay 800 ms  <- pacing
 *               Timeout:VacuumDelay          150 ms  <- pacing
 *               Client:ClientRead            600 ms
 *               Activity:CheckpointerMain    900 ms  <- idle AND hidden
 *
 *   DB Time   = 300+200+50+400+100+150 = 1200 ms
 *   Timeout class row = PgSleep + SpinDelay = 500 ms  (NOT 1450)
 *   Idle      = 800+150+600+900 = 2450 ms
 *   visible idle children = CheckpointWriteDelay 800, ClientRead 600,
 *                           VacuumDelay 150  (Activity is hidden: no row)
 */
#define FIX_DB_TIME_MS   1200.0
#define FIX_TIMEOUT_MS    500.0
#define FIX_IDLE_MS      2450.0
#define FIX_WALL_MS      3000.0

static int build_fixture(struct pgwt_trace_event *ev)
{
    int n = 0;
    uint64_t t = 1000ULL * 1000000000ULL;
    ev[n++] = mk(t +  1 * MS, 101, EV_LOCK,             300, PGWT_CPU_NS_UNKNOWN);
    ev[n++] = mk(t +  2 * MS, 101, EV_IO,               200, PGWT_CPU_NS_UNKNOWN);
    ev[n++] = mk(t +  3 * MS, 101, EV_CLIENTWRITE,       50, PGWT_CPU_NS_UNKNOWN);
    ev[n++] = mk(t +  4 * MS, 102, EV_PGSLEEP,          400, PGWT_CPU_NS_UNKNOWN);
    ev[n++] = mk(t +  5 * MS, 102, EV_SPINDELAY,        100, PGWT_CPU_NS_UNKNOWN);
    ev[n++] = mk(t +  6 * MS, 101, 0,                   150, 120 * MS);
    ev[n++] = mk(t +  7 * MS, 103, EV_CHECKPOINT_DELAY, 800, PGWT_CPU_NS_UNKNOWN);
    ev[n++] = mk(t +  8 * MS, 103, EV_VACUUM_DELAY,     150, PGWT_CPU_NS_UNKNOWN);
    ev[n++] = mk(t +  9 * MS, 101, EV_CLIENTREAD,       600, PGWT_CPU_NS_UNKNOWN);
    ev[n++] = mk(t + 10 * MS, 103, EV_ACTIVITY,         900, PGWT_CPU_NS_UNKNOWN);
    return n;
}

/* ── 1. DB Time and the class rows ─────────────────────────────────────── */
static void test_db_time_excludes_pacing(void)
{
    printf("--- 1. DB Time excludes pacing, keeps PgSleep/SpinDelay ---\n");
    struct pgwt_trace_event ev[16];
    int n = build_fixture(ev);
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(ev, n, &f, 0, 0, FIX_WALL_MS, &tm);

    /* NON-VACUITY FIRST. Everything below is about time being EXCLUDED, and
     * "0 excluded == 0 expected" is the way that passes while broken. Assert
     * the fixture actually contains pacing time before asserting anything
     * about its absence. */
    CHECK(tm.idle_time_ms > 0 && NEAR(tm.idle_time_ms, FIX_IDLE_MS),
          "fixture has %.1f ms idle, expected %.1f (non-vacuity precondition)",
          tm.idle_time_ms, FIX_IDLE_MS);

    CHECK(NEAR(tm.db_time_ms, FIX_DB_TIME_MS),
          "db_time_ms=%.1f expected %.1f (1450 would mean the pacing sleeps "
          "are still counted; 800 would mean the whole Timeout class was "
          "dropped)", tm.db_time_ms, FIX_DB_TIME_MS);
    CHECK(NEAR(tm.aas, FIX_DB_TIME_MS / FIX_WALL_MS),
          "aas=%.4f expected %.4f", tm.aas, FIX_DB_TIME_MS / FIX_WALL_MS);

    /* The Timeout CLASS row is a STRICT SUBSET of the Timeout time present:
     * PgSleep + SpinDelay, not all 1450 ms, and not 0. This is the assertion
     * that distinguishes "six events are pacing" from "the class is idle". */
    const struct pgwt_tm_row *to_row = row_at(&tm, "Timeout", 1);
    CHECK(to_row != NULL, "a Timeout class row must exist");
    if (to_row)
        CHECK(NEAR(to_row->time_ms, FIX_TIMEOUT_MS),
              "Timeout class row=%.1f ms expected %.1f", to_row->time_ms,
              FIX_TIMEOUT_MS);

    /* Client class: ClientWrite only (ClientRead went to Idle). */
    const struct pgwt_tm_row *cl = row_at(&tm, "Client", 1);
    CHECK(cl != NULL && NEAR(cl->time_ms, 50.0),
          "Client class row=%.1f ms expected 50.0", cl ? cl->time_ms : -1);
    /* Activity class must not appear at all: every Activity event is idle. */
    CHECK(row_at(&tm, "Activity", 1) == NULL,
          "no Activity class row (all Activity time is idle)");

    free(tm.rows);
}

/* ── 2. the pacing events stay VISIBLE ─────────────────────────────────── */
static void test_pacing_rows_visible(void)
{
    printf("--- 2. pacing events stay visible ---\n");
    struct pgwt_trace_event ev[16];
    int n = build_fixture(ev);
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    /* 2a. time model sub-event rows: PgSleep and SpinDelay under Timeout. */
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(ev, n, &f, 0, 0, FIX_WALL_MS, &tm);
    const struct pgwt_tm_row *r = row_at(&tm, "Timeout:PgSleep", 2);
    CHECK(r != NULL && NEAR(r->time_ms, 400.0),
          "Timeout:PgSleep sub-row present with 400 ms (got %.1f)",
          r ? r->time_ms : -1);
    r = row_at(&tm, "Timeout:SpinDelay", 2);
    CHECK(r != NULL && NEAR(r->time_ms, 100.0),
          "Timeout:SpinDelay sub-row present with 100 ms (got %.1f)",
          r ? r->time_ms : -1);
    free(tm.rows);

    /* 2b. Top Events: the pacing events are LISTED, carry their real time, and
     * are flagged with the idle %DB sentinel instead of a share of a total
     * they are not part of. Exactly what ClientRead already does. */
    struct pgwt_events_result er;
    pgwt_compute_top_events(ev, n, &f, 0, 0, FIX_WALL_MS, &er);

    const struct pgwt_event_row *cwd = ev_row(&er, EV_CHECKPOINT_DELAY);
    CHECK(cwd != NULL, "Timeout:CheckpointWriteDelay is LISTED in Top Events");
    if (cwd) {
        CHECK(NEAR(cwd->total_ms, 800.0),
              "CheckpointWriteDelay total_ms=%.1f expected 800", cwd->total_ms);
        CHECK(PGWT_PCT_DB_IS_IDLE(cwd->pct_db),
              "CheckpointWriteDelay pct_db=%.2f must be the idle sentinel",
              cwd->pct_db);
        CHECK(strcmp(cwd->name, "Timeout:CheckpointWriteDelay") == 0,
              "it renders its real name, got \"%s\"", cwd->name);
    }
    const struct pgwt_event_row *vd = ev_row(&er, EV_VACUUM_DELAY);
    CHECK(vd != NULL && PGWT_PCT_DB_IS_IDLE(vd->pct_db),
          "Timeout:VacuumDelay listed with the idle sentinel");
    const struct pgwt_event_row *crd = ev_row(&er, EV_CLIENTREAD);
    CHECK(crd != NULL && PGWT_PCT_DB_IS_IDLE(crd->pct_db),
          "Client:ClientRead unchanged: listed with the idle sentinel");
    /* The DB-Time Timeout events get a real percentage. */
    const struct pgwt_event_row *ps = ev_row(&er, EV_PGSLEEP);
    CHECK(ps != NULL && !PGWT_PCT_DB_IS_IDLE(ps->pct_db) && ps->pct_db > 0,
          "Timeout:PgSleep gets a real %%DB (got %.2f)", ps ? ps->pct_db : -1);
    /* Top Events' DB Time must match the time model's. */
    CHECK(NEAR(er.db_time_ms, FIX_DB_TIME_MS),
          "top_events db_time_ms=%.1f expected %.1f", er.db_time_ms,
          FIX_DB_TIME_MS);
    /* And the hidden Activity event is STILL hidden — the visibility rule did
     * not move. */
    CHECK(ev_row(&er, EV_ACTIVITY) == NULL,
          "Activity:CheckpointerMain stays hidden from Top Events");
    free(er.rows);
}


/* ── 2b. VISIBLE also means transitions and the latency heatmap ────────────
 *
 * Criterion 4 of the task names timelines, transitions and histograms as well
 * as event lists. Those views filter on pgwt_is_hidden_event, which this change
 * did not touch, so they are correct by construction -- but "correct by
 * construction" is an argument, and an argument is what a later refactor
 * silently invalidates. A line added to pgwt_compute_transitions that skipped
 * idle events would delete the checkpointer's whole state machine from the
 * graph and nothing else in this file would notice. So they are asserted.
 *
 * Both are checked against the SAME fixture as section 2, and the contrast is
 * the Activity event: it must be absent from both, which proves the assertions
 * distinguish "visible-idle" from "idle" rather than just finding every event.
 */
static void test_pacing_in_transitions_and_heatmap(void)
{
    printf("--- 2b. pacing events in transitions and the heatmap ---\n");
    struct pgwt_trace_event ev[16];
    int n = build_fixture(ev);
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    /* TRANSITIONS. build_fixture gives every record new_event == EV_IO, so each
     * record is one <old_event> -> IO edge. */
    struct pgwt_transitions_result tr;
    pgwt_compute_transitions(ev, n, &f, 64, &tr);
    CHECK(tr.num_rows > 0, "the fixture produces transitions (%d rows)",
          tr.num_rows);
    int found_cwd = 0, found_vd = 0, found_cr = 0, found_activity = 0;
    double cwd_ns = 0;
    for (int i = 0; i < tr.num_rows; i++) {
        if (tr.rows[i].from_event == EV_CHECKPOINT_DELAY) {
            found_cwd = 1; cwd_ns = tr.rows[i].total_ns;
        }
        if (tr.rows[i].from_event == EV_VACUUM_DELAY)  found_vd = 1;
        if (tr.rows[i].from_event == EV_CLIENTREAD)    found_cr = 1;
        if (tr.rows[i].from_event == EV_ACTIVITY ||
            tr.rows[i].to_event   == EV_ACTIVITY)     found_activity = 1;
    }
    CHECK(found_cwd,
          "Timeout:CheckpointWriteDelay keeps its edge in the transition graph "
          "-- excluded from load, not from the state machine");
    CHECK(NEAR(cwd_ns, 800.0 * 1e6),
          "...carrying its real 800 ms (got %.1f ms)", cwd_ns / 1e6);
    CHECK(found_vd, "Timeout:VacuumDelay too");
    CHECK(found_cr, "Client:ClientRead unchanged (the precedent)");
    CHECK(!found_activity,
          "...and the HIDDEN Activity event is still absent, so these "
          "assertions distinguish visible-idle from idle");
    free(tr.rows);

    /* HEATMAP (the latency histogram over time). Filtered to the pacing event:
     * a hidden-event filter would come back empty. Unlike the other entry
     * points this one REQUIRES a real window (it returns immediately when
     * to_ns == from_ns), so the fixture's own second is passed explicitly --
     * a 0,0 call here would report 0 events for every event and the contrast
     * below would pass vacuously. */
    const uint64_t HM_FROM = 1000ULL * 1000000000ULL;
    const uint64_t HM_TO   = HM_FROM + 1000000000ULL;
    struct pgwt_filter hf;
    memset(&hf, 0, sizeof(hf));
    hf.event_id = EV_CHECKPOINT_DELAY;
    struct pgwt_heatmap_result hm;
    pgwt_compute_heatmap(ev, n, &hf, HM_FROM, HM_TO, 8, &hm);
    CHECK(hm.total_events == 1,
          "the pacing event appears in the latency heatmap (%llu events)",
          (unsigned long long)hm.total_events);
    free(hm.grid); free(hm.times);

    /* The contrast: the same request for the HIDDEN event must come back
     * empty, which is what makes the count above mean "visible". */
    memset(&hf, 0, sizeof(hf));
    hf.event_id = EV_ACTIVITY;
    pgwt_compute_heatmap(ev, n, &hf, HM_FROM, HM_TO, 8, &hm);
    CHECK(hm.total_events == 0,
          "the hidden Activity event does NOT (%llu events)",
          (unsigned long long)hm.total_events);
    free(hm.grid); free(hm.times);

    /* ...and the empty result above is because the event is HIDDEN, not
     * because the window missed it: the same window unfiltered sees every
     * non-hidden record in the fixture (9 of the 10; the Activity one is the
     * exception). Without this the contrast would also pass with a window
     * that contains nothing at all. */
    struct pgwt_filter nf;
    memset(&nf, 0, sizeof(nf));
    pgwt_compute_heatmap(ev, n, &nf, HM_FROM, HM_TO, 8, &hm);
    CHECK(hm.total_events == 9,
          "the same window unfiltered holds 9 visible records (got %llu) -- so "
          "the Activity 0 above is hiddenness, not an empty window",
          (unsigned long long)hm.total_events);
    free(hm.grid); free(hm.times);
}

/* ── 3. the Idle row and its NAMED children ────────────────────────────────
 *
 * INDENTS ARE LOAD-BEARING, and not only cosmetically:
 *
 *  - tests/demo_rehearsal_lib.py time_model_conservation() sums EVERY
 *    indent==1 row and requires the total to equal db_time_ms. Idle is by
 *    definition not in db_time_ms, so an indent-1 Idle row breaks that gate on
 *    contact. Hence indent 0 — which is also what
 *    web/static/views/overview.js already looks for
 *    (`r.indent === 0 && r.name.indexOf('Idle') >= 0`) and what
 *    tests/mock_server.py has shipped in its fixture all along.
 *  - the children are at indent 2, which is where
 *    demo_rehearsal_lib.REQUIRED_WORKLOAD_EVENTS reads sub-event rows.
 */
static void test_idle_row_named_children(void)
{
    printf("--- 3. Idle row with named children ---\n");
    struct pgwt_trace_event ev[16];
    int n = build_fixture(ev);
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(ev, n, &f, 0, 0, FIX_WALL_MS, &tm);

    const struct pgwt_tm_row *idle = row_at(&tm, "Idle", 0);
    CHECK(idle != NULL, "an indent-0 \"Idle\" row must exist");
    if (idle) {
        CHECK(NEAR(idle->time_ms, FIX_IDLE_MS),
              "Idle row=%.1f ms expected %.1f", idle->time_ms, FIX_IDLE_MS);
        CHECK(NEAR(idle->time_ms, tm.idle_time_ms),
              "the Idle ROW and idle_time_ms must agree (%.1f vs %.1f)",
              idle->time_ms, tm.idle_time_ms);
        /* No share of a total it is not part of, and no AAS: reporting 68%% of
         * DB Time next to a row excluded from DB Time is how an excluded
         * number gets added back into a reader's mental sum. */
        CHECK(idle->pct_db_time == 0.0,
              "Idle pct_db_time=%.2f must be 0", idle->pct_db_time);
        CHECK(idle->aas == 0.0, "Idle aas=%.2f must be 0", idle->aas);
    }

    /* The whole point: the number is EXPLAINED. */
    const struct pgwt_tm_row *c1 = row_at(&tm, "Timeout:CheckpointWriteDelay", 2);
    CHECK(c1 != NULL && NEAR(c1->time_ms, 800.0),
          "named child Timeout:CheckpointWriteDelay = 800 ms (got %.1f)",
          c1 ? c1->time_ms : -1);
    const struct pgwt_tm_row *c2 = row_at(&tm, "Client:ClientRead", 2);
    CHECK(c2 != NULL && NEAR(c2->time_ms, 600.0),
          "named child Client:ClientRead = 600 ms (got %.1f)",
          c2 ? c2->time_ms : -1);
    const struct pgwt_tm_row *c3 = row_at(&tm, "Timeout:VacuumDelay", 2);
    CHECK(c3 != NULL && NEAR(c3->time_ms, 150.0),
          "named child Timeout:VacuumDelay = 150 ms (got %.1f)",
          c3 ? c3->time_ms : -1);
    if (c1) CHECK(c1->pct_db_time == 0.0 && c1->aas == 0.0,
                  "idle children carry no %%DB and no AAS");

    /* Sorted descending, like every other breakdown in the model. */
    int i1 = -1, i2 = -1, i3 = -1;
    for (int i = 0; i < tm.num_rows; i++) {
        if (c1 && &tm.rows[i] == c1) i1 = i;
        if (c2 && &tm.rows[i] == c2) i2 = i;
        if (c3 && &tm.rows[i] == c3) i3 = i;
    }
    CHECK(i1 >= 0 && i2 > i1 && i3 > i2,
          "idle children are ordered by time descending (800, 600, 150): "
          "indices %d %d %d", i1, i2, i3);

    /* HIDDEN idle stays inside the parent and gets NO row. Activity-class
     * events are hidden by pgwt_is_hidden_event and that rule did not change;
     * a 900 ms "Activity:CheckpointerMain" row would be a visibility
     * regression smuggled in by this change. */
    CHECK(row_by_name(&tm, "Activity:CheckpointerMain") == NULL,
          "the hidden Activity event gets no row");
    /* Its time IS in the parent total, so the children do not have to sum to
     * it — assert the relationship explicitly so a future reader does not
     * "fix" it. */
    double kids = 0;
    for (int i = 0; i < tm.num_rows; i++)
        if (tm.rows[i].indent == 2 &&
            (strcmp(tm.rows[i].name, "Timeout:CheckpointWriteDelay") == 0 ||
             strcmp(tm.rows[i].name, "Client:ClientRead") == 0 ||
             strcmp(tm.rows[i].name, "Timeout:VacuumDelay") == 0))
            kids += tm.rows[i].time_ms;
    CHECK(NEAR(kids, 1550.0) && NEAR(FIX_IDLE_MS - kids, 900.0),
          "visible children sum to %.1f; the %.1f ms remainder is the hidden "
          "Activity time", kids, FIX_IDLE_MS - kids);

    /* rows[0] is still "DB Time": overview.js reads data.rows[0] directly. */
    CHECK(tm.num_rows > 0 && strcmp(tm.rows[0].name, "DB Time") == 0 &&
          tm.rows[0].indent == 0,
          "rows[0] is still the indent-0 DB Time row");
    /* THE STRUCTURAL TRAP: no indent-1 row may be named Idle, and there must
     * be exactly two indent-0 rows (DB Time, Idle). An extra indent-0 row or
     * an indent-1 Idle row each break a gate in a file this branch must not
     * edit. */
    CHECK(row_at(&tm, "Idle", 1) == NULL,
          "no indent-1 Idle row (it would break "
          "demo_rehearsal_lib.time_model_conservation)");
    CHECK(count_indent(&tm, 0) == 2,
          "exactly 2 indent-0 rows (DB Time, Idle), got %d",
          count_indent(&tm, 0));

    free(tm.rows);
}

/* ── 4. conservation, by COMPONENT ─────────────────────────────────────── */
static void test_conservation_components(void)
{
    printf("--- 4. conservation: components, not just the identity ---\n");
    struct pgwt_trace_event ev[16];
    int n = build_fixture(ev);
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(ev, n, &f, 0, 0, FIX_WALL_MS, &tm);

    /* The identity the docs state (ROADMAP_AND_STATUS "identity holds by
     * construction") and the one demo_rehearsal_lib checks: indent-1 rows sum
     * to DB Time. */
    CHECK(NEAR(sum_indent(&tm, 1), tm.db_time_ms),
          "sum(indent==1)=%.4f must equal db_time_ms=%.4f",
          sum_indent(&tm, 1), tm.db_time_ms);

    /* ...and now the components, because the identity above CANNOT fail when
     * time goes missing: Off-CPU* is the residual DB Time - CPU* - sum(waits),
     * so unattributed time lands there and the sum still closes. Each of these
     * is a number read off the fixture. */
    CHECK(tm.has_measured_cpu, "the fixture has measured cpu_ns");
    CHECK(NEAR(tm.cpu_ms, 120.0),
          "CPU* = the measured 120 ms, got %.1f", tm.cpu_ms);
    CHECK(NEAR(tm.offcpu_ms, 30.0),
          "Off-CPU* = 150 ms gap - 120 ms measured = 30 ms, got %.1f",
          tm.offcpu_ms);
    double waits = 0;
    const char *wait_classes[] = {"IO", "Lock", "Client", "Timeout"};
    double want[] = {200.0, 300.0, 50.0, 500.0};
    for (int i = 0; i < 4; i++) {
        const struct pgwt_tm_row *r = row_at(&tm, wait_classes[i], 1);
        CHECK(r != NULL && NEAR(r->time_ms, want[i]),
              "%s class = %.1f ms, got %.1f", wait_classes[i], want[i],
              r ? r->time_ms : -1);
        if (r) waits += r->time_ms;
    }
    CHECK(NEAR(waits, 1050.0), "wait classes sum to 1050 ms, got %.1f", waits);
    CHECK(NEAR(tm.cpu_ms + tm.offcpu_ms + waits, tm.db_time_ms),
          "CPU* + Off-CPU* + sum(waits) = %.4f must equal DB Time %.4f",
          tm.cpu_ms + tm.offcpu_ms + waits, tm.db_time_ms);
    CHECK(tm.cpu_clamped_ms == 0.0,
          "no clamping on this fixture (got %.4f)", tm.cpu_clamped_ms);

    /* DB Time and Idle must partition the measured time with nothing lost:
     * every record in the fixture is in exactly one of them. */
    double total_wall = 300+200+50+400+100+150+800+150+600+900;
    CHECK(NEAR(tm.db_time_ms + tm.idle_time_ms, total_wall),
          "DB Time %.1f + Idle %.1f must equal the %.1f ms the fixture "
          "contains — nothing silently dropped", tm.db_time_ms,
          tm.idle_time_ms, total_wall);
    free(tm.rows);
}

/* ── 5. CPU recorded ON and ADJACENT TO a pacing wait ──────────────────────
 *
 * src/compute.c skips an idle interval WHOLE, including its measured cpu_ns.
 * That is what ClientRead has always done; this section asserts (a) what the
 * numbers are, and (b) that pacing and ClientRead come out IDENTICAL, which is
 * the actual content of "we followed the precedent".
 *
 * The leak this documents is PRE-EXISTING and deliberately not fixed here:
 * se.sum_exec_runtime is only current at a tick or a context switch, so a
 * sub-ms CPU burst just before a wait is attributed to the FOLLOWING interval.
 * Checkpointer write CPU therefore lands in the CheckpointWriteDelay span that
 * follows it and is now dropped from CPU* — exactly as backend CPU before a
 * ClientRead already was. It surfaces in wait_gap_cpu_ms, the trace's own
 * self-check, which is asserted here so the quantity stays observable.
 */
static void cpu_adjacent_case(uint32_t idle_ev, const char *label,
                              double *out_cpu, double *out_db,
                              double *out_idle, double *out_gapcpu)
{
    struct pgwt_trace_event ev[4];
    int n = 0;
    uint64_t t = 2000ULL * 1000000000ULL;
    /* An on-CPU gap with real measured CPU, immediately BEFORE the idle wait. */
    ev[n++] = mk(t + 1 * MS, 201, 0,       100,  90 * MS);
    /* The idle wait itself, carrying measured CPU (spillover). */
    ev[n++] = mk(t + 2 * MS, 201, idle_ev, 500,  40 * MS);
    /* A non-idle wait after it, also carrying measured CPU. */
    ev[n++] = mk(t + 3 * MS, 201, EV_LOCK, 200,  10 * MS);
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(ev, n, &f, 0, 0, 1000.0, &tm);
    *out_cpu    = tm.cpu_ms;
    *out_db     = tm.db_time_ms;
    *out_idle   = tm.idle_time_ms;
    *out_gapcpu = tm.wait_gap_cpu_ms;

    CHECK(NEAR(tm.db_time_ms, 300.0),
          "%s: DB Time = 100 (CPU gap) + 200 (Lock) = 300, got %.1f",
          label, tm.db_time_ms);
    CHECK(NEAR(tm.idle_time_ms, 500.0),
          "%s: Idle = the 500 ms idle wall, got %.1f", label, tm.idle_time_ms);
    CHECK(NEAR(tm.cpu_ms, 90.0),
          "%s: CPU* = 90 ms (the idle interval's 40 ms of measured CPU is "
          "DROPPED with the interval; the Lock interval's 10 ms stays with its "
          "label), got %.1f", label, tm.cpu_ms);
    CHECK(NEAR(tm.offcpu_ms, 10.0),
          "%s: Off-CPU* = 300 - 90 - 200 = 10 ms, got %.1f", label,
          tm.offcpu_ms);
    CHECK(NEAR(tm.wait_gap_cpu_ms, 10.0),
          "%s: wait_gap_cpu_ms = 10 ms — only the NON-idle wait's CPU is "
          "observable here; the 40 ms inside the idle span is not even "
          "counted as an observability sum, got %.1f",
          label, tm.wait_gap_cpu_ms);
    CHECK(tm.cpu_clamped_ms == 0.0, "%s: no clamp (got %.4f)", label,
          tm.cpu_clamped_ms);
    free(tm.rows);
}

static void test_cpu_on_and_adjacent_to_pacing(void)
{
    printf("--- 5. CPU on and adjacent to a pacing wait ---\n");
    double c1, d1, i1, g1, c2, d2, i2, g2;
    cpu_adjacent_case(EV_CHECKPOINT_DELAY, "pacing", &c1, &d1, &i1, &g1);
    cpu_adjacent_case(EV_CLIENTREAD,       "ClientRead", &c2, &d2, &i2, &g2);
    /* THE PRECEDENT, asserted rather than claimed: a pacing sleep is treated
     * exactly as ClientRead is. If a future change special-cases one of them,
     * this is where it shows. */
    CHECK(NEAR(c1, c2) && NEAR(d1, d2) && NEAR(i1, i2) && NEAR(g1, g2),
          "pacing and ClientRead must be accounted identically: "
          "cpu %.1f/%.1f db %.1f/%.1f idle %.1f/%.1f gapcpu %.1f/%.1f",
          c1, c2, d1, d2, i1, i2, g1, g2);

    /* CLAMP: measured CPU larger than the interval's wall is a clock-skew
     * inconsistency and must be counted, on the CPU gap. Put the oversized
     * cpu_ns on the on-CPU gap (where the clamp lives) and check it surfaces
     * instead of inflating CPU* past the wall. */
    struct pgwt_trace_event ev[3];
    int n = 0;
    uint64_t t = 3000ULL * 1000000000ULL;
    ev[n++] = mk(t + 1 * MS, 301, 0,                   100, 180 * MS);
    ev[n++] = mk(t + 2 * MS, 301, EV_CHECKPOINT_DELAY, 500, 400 * MS);
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(ev, n, &f, 0, 0, 1000.0, &tm);
    CHECK(NEAR(tm.cpu_ms, 100.0),
          "CPU* clamped to the 100 ms gap, got %.1f", tm.cpu_ms);
    CHECK(NEAR(tm.cpu_clamped_ms, 80.0),
          "cpu_clamped_ms = 80 ms (180 measured - 100 wall), got %.1f",
          tm.cpu_clamped_ms);
    CHECK(NEAR(tm.db_time_ms, 100.0) && NEAR(tm.idle_time_ms, 500.0),
          "the pacing interval's 400 ms of CPU does not leak into DB Time "
          "(db=%.1f idle=%.1f)", tm.db_time_ms, tm.idle_time_ms);
    free(tm.rows);
}

/* ══ 6. FALSE NEGATIVES ═══════════════════════════════════════════════════
 * Every section above asserts that time was EXCLUDED. "0 ms excluded == 0 ms
 * expected" satisfies all of it. These are the ways this file could be unable
 * to see what it is checking. */
static void test_bypass_suite(void)
{
    printf("--- 6. bypass suite: ways this file could stop seeing ---\n");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;

    /* 6a. EMPTY INPUT. No rows, no Idle row, DB Time 0 — and crucially the
     * Idle row is ABSENT rather than present-and-zero, so "Idle exists" in
     * section 3 is evidence that something was idle. */
    pgwt_compute_time_model(NULL, 0, &f, 0, 0, FIX_WALL_MS, &tm);
    CHECK(tm.db_time_ms == 0.0 && tm.idle_time_ms == 0.0,
          "empty input: db=%.1f idle=%.1f", tm.db_time_ms, tm.idle_time_ms);
    CHECK(row_at(&tm, "Idle", 0) == NULL,
          "empty input emits NO Idle row (so section 3's Idle row proves "
          "idle time was observed, not that the row is unconditional)");
    CHECK(tm.num_rows == 1 && strcmp(tm.rows[0].name, "DB Time") == 0,
          "empty input emits the DB Time row only, got %d rows", tm.num_rows);
    free(tm.rows);

    /* 6b. THE THING BEING CHECKED IS ABSENT, not wrong. A fixture with no
     * Timeout event at all makes every "pacing is excluded" assertion
     * vacuously true. Pin that it is DISTINGUISHABLE: DB Time equals the whole
     * fixture and there are no Timeout rows anywhere. A test relying on this
     * shape would be blind, which is why section 1 asserts idle_time_ms > 0
     * first. */
    struct pgwt_trace_event no_to[2];
    int n = 0;
    uint64_t t = 4000ULL * 1000000000ULL;
    no_to[n++] = mk(t + 1 * MS, 401, EV_LOCK, 300, PGWT_CPU_NS_UNKNOWN);
    no_to[n++] = mk(t + 2 * MS, 401, EV_IO,   200, PGWT_CPU_NS_UNKNOWN);
    pgwt_compute_time_model(no_to, n, &f, 0, 0, FIX_WALL_MS, &tm);
    CHECK(NEAR(tm.db_time_ms, 500.0) && tm.idle_time_ms == 0.0,
          "no-Timeout fixture: db=%.1f idle=%.1f — a fixture like this cannot "
          "detect anything about pacing", tm.db_time_ms, tm.idle_time_ms);
    CHECK(row_at(&tm, "Timeout", 1) == NULL && row_at(&tm, "Idle", 0) == NULL,
          "no-Timeout fixture has neither a Timeout class row nor an Idle row");
    free(tm.rows);

    /* 6c. PRESENT BUT ZERO-DURATION. The pacing event occurs, so a
     * "is the event in the stream" check would pass, but there is no time to
     * exclude. The Idle row must then be absent rather than a 0 ms row
     * pretending the breakdown worked. */
    struct pgwt_trace_event zero[2];
    n = 0;
    zero[n++] = mk(t + 1 * MS, 402, EV_CHECKPOINT_DELAY, 0, PGWT_CPU_NS_UNKNOWN);
    zero[n++] = mk(t + 2 * MS, 402, EV_LOCK,           300, PGWT_CPU_NS_UNKNOWN);
    pgwt_compute_time_model(zero, n, &f, 0, 0, FIX_WALL_MS, &tm);
    CHECK(NEAR(tm.db_time_ms, 300.0) && tm.idle_time_ms == 0.0,
          "zero-duration pacing event: db=%.1f idle=%.1f", tm.db_time_ms,
          tm.idle_time_ms);
    CHECK(row_at(&tm, "Idle", 0) == NULL,
          "a 0 ms idle total emits no Idle row");
    free(tm.rows);

    /* 6d. A WINDOW THAT CLIPS EVERYTHING AWAY. The fixture is full of pacing
     * time but the requested window contains none of it, so every number is 0.
     * An agreement test run on such a window is void evidence; pin that it
     * really does come back empty rather than silently using whole-capture
     * totals. */
    struct pgwt_trace_event ev[16];
    int fn = build_fixture(ev);
    uint64_t far_from = 9000ULL * 1000000000ULL;
    uint64_t far_to   = far_from + 1000ULL * 1000000ULL;
    pgwt_compute_time_model(ev, fn, &f, far_from, far_to, 1000.0, &tm);
    CHECK(tm.db_time_ms == 0.0 && tm.idle_time_ms == 0.0,
          "a window past the data clips to nothing (db=%.1f idle=%.1f)",
          tm.db_time_ms, tm.idle_time_ms);
    free(tm.rows);

    /* 6e. THE MASK IS EMPTY. If the Timeout pacing mask were ever installed as
     * 0 (an id list that matched nothing on this version — the PG13 failure
     * mode), DB Time silently returns to the old number and NOTHING in sections
     * 1-5 would name the cause, because they would all just be "wrong by
     * 950 ms". Drive it directly so the failure has a name, then restore. */
    uint32_t saved = pgwt_idle_rule_timeout_mask();
    pgwt_idle_rule_set_timeout_mask(0);
    pgwt_compute_time_model(ev, fn, &f, 0, 0, FIX_WALL_MS, &tm);
    CHECK(NEAR(tm.db_time_ms, FIX_DB_TIME_MS + 950.0),
          "with an EMPTY pacing mask DB Time returns to %.1f ms — this is the "
          "number a hardcoded-id implementation reports on PG13 (got %.1f)",
          FIX_DB_TIME_MS + 950.0, tm.db_time_ms);
    CHECK(!NEAR(tm.db_time_ms, FIX_DB_TIME_MS),
          "and it is DIFFERENT from the correct %.1f ms, so sections 1-5 can "
          "actually go red", FIX_DB_TIME_MS);
    free(tm.rows);
    pgwt_idle_rule_set_timeout_mask(saved);
    CHECK(pgwt_idle_rule_timeout_mask() == saved, "mask restored");
    /* Prove the restore worked, so a later section is not silently running on
     * the mutated mask. */
    pgwt_compute_time_model(ev, fn, &f, 0, 0, FIX_WALL_MS, &tm);
    CHECK(NEAR(tm.db_time_ms, FIX_DB_TIME_MS),
          "after restore, DB Time is %.1f again (got %.1f)", FIX_DB_TIME_MS,
          tm.db_time_ms);
    free(tm.rows);

    /* 6f. MORE VISIBLE IDLE EVENTS THAN THE CHILD-ROW BUDGET. There are 7
     * possible visible idle events today (ClientRead + the six pacing sleeps);
     * the budget is MAX_IDLE_SUB_ROWS = 8. Build all 7 and assert the PARENT
     * total still equals their sum — a truncated breakdown must never silently
     * change the Idle total. */
    struct pgwt_trace_event many[8];
    n = 0;
    const uint32_t vis_idle[] = {
        WEI(PG_WAIT_TIMEOUT, 0), WEI(PG_WAIT_TIMEOUT, 1),
        WEI(PG_WAIT_TIMEOUT, 3), WEI(PG_WAIT_TIMEOUT, 4),
        WEI(PG_WAIT_TIMEOUT, 7), WEI(PG_WAIT_TIMEOUT, 9),
        EV_CLIENTREAD,
    };
    double expect_idle = 0;
    for (size_t i = 0; i < sizeof(vis_idle)/sizeof(vis_idle[0]); i++) {
        CHECK(pgwt_is_idle_event(vis_idle[i]) != 0 &&
              pgwt_is_hidden_event(vis_idle[i]) == 0,
              "fixture event %zu is visible-idle", i);
        many[n++] = mk(t + (uint64_t)(i + 1) * MS, 403, vis_idle[i],
                       10 * (i + 1), PGWT_CPU_NS_UNKNOWN);
        expect_idle += 10.0 * (double)(i + 1);
    }
    pgwt_compute_time_model(many, n, &f, 0, 0, FIX_WALL_MS, &tm);
    CHECK(NEAR(tm.idle_time_ms, expect_idle),
          "7 visible idle events: Idle total %.1f expected %.1f",
          tm.idle_time_ms, expect_idle);
    CHECK(count_indent(&tm, 2) == 7,
          "all 7 get a named child row, got %d", count_indent(&tm, 2));
    CHECK(NEAR(sum_indent(&tm, 2), expect_idle),
          "children sum to the parent when nothing is hidden (%.1f vs %.1f)",
          sum_indent(&tm, 2), expect_idle);
    CHECK(tm.db_time_ms == 0.0, "all-idle fixture has DB Time 0 (got %.1f)",
          tm.db_time_ms);
    free(tm.rows);
}

int main(void)
{
    printf("=== test_idle_accounting ===\n");
    pgwt_init_event_names(18);
    /* Guard the whole file: every event id below is a PG18 id and the
     * assertions are meaningless if the active tables are not PG18. */
    if (pgwt_idle_rule_timeout_mask() != PGWT_IDLE_TIMEOUT_MASK_PG18) {
        printf("REFUSING: PG18 tables not active (mask 0x%03x) — this file's "
               "event ids would mean something else\n",
               pgwt_idle_rule_timeout_mask());
        return 1;
    }
    test_db_time_excludes_pacing();
    test_pacing_rows_visible();
    test_pacing_in_transitions_and_heatmap();
    test_idle_row_named_children();
    test_conservation_components();
    test_cpu_on_and_adjacent_to_pacing();
    test_bypass_suite();

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
