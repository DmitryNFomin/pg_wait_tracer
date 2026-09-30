/* test_compute_orphan_close.c — pgwt_compute_executions' orphan-row close.
 *
 * A PostgreSQL execution that ERRORs, is cancelled, hits statement_timeout
 * or disconnects mid-query never reaches query__execute__done: the longjmp
 * skips it, so no EXEC_END marker is ever emitted. Before this, the row
 * stayed in_progress for the whole retained capture and its elapsed-so-far
 * grew without bound.
 *
 * The fix closes such a row at the pid's next observed IDLE boundary in the
 * WAIT stream (full mode captures that for every traced pid) or at its
 * process exit. This test pins:
 *   1. the cancel case actually closes, with end_inferred set;
 *   2. pid reuse cannot cross-close (old row / new row, either direction);
 *   3. with no idle boundary and no exit the row stays visibly unresolved
 *      rather than being closed with a fabricated duration;
 *   4. a measured EXEC_END always wins over a deduced boundary (COPY FROM
 *      STDIN legitimately waits on Client:ClientRead INSIDE its execution);
 *   5. an idle boundary CONTRADICTED by later work is never used at exit
 *      or at end of stream: the execution may still be running, and a
 *      stale close under-states the duration the Waterfall ranks on;
 *   6. every way the close could be made UNREACHABLE or could fire when
 *      there is nothing to close (the bypass suite at the bottom).
 *
 * Pure compute: no BPF, no PostgreSQL, no trace files. Runs anywhere.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "../src/compute.h"
#include "../src/summary_reader.h"
#include "../src/pg_wait_tracer.h"

/* compute.c's summary-path helpers are not exercised here; stub the one
 * foreign symbol so this links standalone (same pattern as
 * test_live_accum.c). */
int pgwt_visit_summaries(const char *dir, uint64_t from_ns, uint64_t to_ns,
                         pgwt_summary_visitor fn, void *ctx);
int pgwt_visit_summaries(const char *dir, uint64_t from_ns, uint64_t to_ns,
                         pgwt_summary_visitor fn, void *ctx)
{
    (void)dir; (void)from_ns; (void)to_ns; (void)fn; (void)ctx;
    return -1;
}

static int checks_run, checks_failed;

static void check(int cond, const char *fmt, ...)
{
    va_list ap;
    checks_run++;
    if (cond) {
        printf("  PASS: ");
    } else {
        checks_failed++;
        printf("  FAIL: ");
    }
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

#define MS      1000000ULL
/* Above 2^53 on purpose: the same range a real capture uses. */
#define BASE    10000000000000000ULL
#define WINDOW_FROM (BASE - 1000 * MS)
#define WINDOW_TO   (BASE + 1000 * MS)

#define CLIENT_READ  WEI(PG_WAIT_CLIENT, 0)     /* idle */
#define CLIENT_WRITE WEI(PG_WAIT_CLIENT, 1)     /* NOT idle */
#define IO_READ      WEI(PG_WAIT_IO, 8)         /* NOT idle */
#define CPU_EVENT    0U

/* ── event builders ──────────────────────────────────────────── */

static struct pgwt_trace_event mark(uint32_t pid, uint64_t ts, uint32_t kind,
                                    uint64_t qid)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;
    e.pid = pid;
    e.old_event = kind;
    e.new_event = kind;
    e.query_id = qid;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return e;
}

/* A closed wait interval: `old` ended at ts after dur ns, `next` begins. */
static struct pgwt_trace_event wait_rec(uint32_t pid, uint64_t ts,
                                        uint64_t dur, uint32_t old,
                                        uint32_t next, uint64_t qid)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;
    e.pid = pid;
    e.old_event = old;
    e.new_event = next;
    e.duration_ns = dur;
    e.query_id = qid;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return e;
}

static struct pgwt_trace_event exit_rec(uint32_t pid, uint64_t ts,
                                        uint32_t old, uint64_t qid)
{
    struct pgwt_trace_event e = wait_rec(pid, ts, MS, old, PGWT_EVENT_EXIT, qid);
    return e;
}

static struct pgwt_trace_event sample_rec(uint32_t pid, uint64_t ts,
                                          uint32_t we, uint64_t qid)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;
    e.pid = pid;
    e.old_event = 0;
    e.new_event = we;
    e.flags = PGWT_EVENT_FLAG_SAMPLE;
    e.query_id = qid;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return e;
}

static void run(const struct pgwt_trace_event *evs, int n,
                struct pgwt_executions_result *out)
{
    pgwt_compute_executions(evs, n, WINDOW_FROM, WINDOW_TO, NULL, NULL, 0, out);
}

/* Find the single row with this start_ns; NULL when absent. */
static const struct pgwt_execution *row_at(
    const struct pgwt_executions_result *r, uint64_t start_ns)
{
    for (int i = 0; i < r->num_rows; i++)
        if (r->rows[i].start_ns == start_ns)
            return &r->rows[i];
    return NULL;
}

/* ── 1. the cancel / ERROR case ──────────────────────────────── */

static void test_cancel_closes_at_idle(void)
{
    printf("\n### 1. a cancelled statement closes at its next idle record ###\n");
    /* EXEC_START, the statement runs, is cancelled -> no EXEC_END ever; the
     * backend returns to Client:ClientRead and stays there. Nothing else in
     * the stream: exactly the presenter-Ctrl-C shape. */
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        wait_rec(4242, BASE + 10 * MS, 10 * MS, CPU_EVENT, IO_READ, 77),
        wait_rec(4242, BASE + 30 * MS, 20 * MS, IO_READ, CPU_EVENT, 77),
        /* the cancel lands: the backend goes back to waiting for its client */
        wait_rec(4242, BASE + 50 * MS, 20 * MS, CPU_EVENT, CLIENT_READ, 77),
    };
    struct pgwt_executions_result r;
    run(evs, 4, &r);

    check(!r.failed, "compute succeeded");
    check(r.num_rows == 1, "the EXEC_START is a row (got %d)", r.num_rows);
    if (r.num_rows == 1) {
        const struct pgwt_execution *row = &r.rows[0];
        check(!row->in_progress, "row is CLOSED, not left in progress");
        check(row->end_ns == BASE + 50 * MS,
              "end is the idle transition's own timestamp (got +%llu ms)",
              (unsigned long long)((row->end_ns - BASE) / MS));
        check(row->end_inferred == 1,
              "the deduced end is LABELLED end_inferred");
        check(row->end_ns - row->start_ns == 50 * MS,
              "duration is 50ms, bounded — not the whole capture");
    }
    free(r.rows);
}

static void test_close_is_not_elapsed_so_far(void)
{
    printf("\n### 1b. the deduced end does not grow with the window ###\n");
    /* The same cancel, with the capture continuing for another 900ms of
     * OTHER pids' traffic. The orphan's duration must not follow it. */
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        wait_rec(4242, BASE + 50 * MS, 50 * MS, CPU_EVENT, CLIENT_READ, 77),
        wait_rec(9999, BASE + 900 * MS, 10 * MS, CPU_EVENT, IO_READ, 5),
    };
    struct pgwt_executions_result r;
    run(evs, 3, &r);
    const struct pgwt_execution *row = row_at(&r, BASE);
    check(row != NULL, "orphan row present");
    if (row)
        check(!row->in_progress && row->end_ns == BASE + 50 * MS,
              "duration stays 50ms while the capture runs on to +900ms");
    free(r.rows);
}

static void test_next_statement_settles_previous(void)
{
    printf("\n### 1c. error, then another statement on the same connection ###\n");
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        /* ERROR -> idle */
        wait_rec(4242, BASE + 20 * MS, 20 * MS, CPU_EVENT, CLIENT_READ, 77),
        /* the client sends the next statement: the idle interval closes */
        wait_rec(4242, BASE + 60 * MS, 40 * MS, CLIENT_READ, CPU_EVENT, 77),
        mark(4242, BASE + 61 * MS, PGWT_MARKER_EXEC_START, 88),
        wait_rec(4242, BASE + 70 * MS, 9 * MS, CPU_EVENT, IO_READ, 88),
        mark(4242, BASE + 80 * MS, PGWT_MARKER_EXEC_END, 88),
    };
    struct pgwt_executions_result r;
    run(evs, 6, &r);
    check(r.num_rows == 2, "two rows (got %d)", r.num_rows);
    const struct pgwt_execution *a = row_at(&r, BASE);
    const struct pgwt_execution *b = row_at(&r, BASE + 61 * MS);
    check(a && !a->in_progress && a->end_inferred == 1 &&
          a->end_ns == BASE + 20 * MS,
          "the errored row closed at ITS idle boundary, labelled inferred");
    check(b && !b->in_progress && b->end_inferred == 0 &&
          b->end_ns == BASE + 80 * MS,
          "the following statement closed at its MEASURED EXEC_END");
    free(r.rows);
}


/* ── 1d. work observed AFTER an intra-execution idle ──────────
 *
 * The idle candidate must be invalidated by later work. Without that, a
 * row that went idle once and then kept working is closed at the idle: a
 * still-running execution reported as a short one, and short is exactly
 * the direction that hides it from a duration-ranked Waterfall.
 *
 * Each case below is paired with a CONTROL that is the same stream minus
 * the post-idle work. The control must CLOSE. That pairing is what keeps
 * these cases from passing vacuously: if the close path were dead — if
 * pgwt_is_idle_event never matched, if the candidate were never set, if
 * the whole feature were reverted — the "stays in_progress" assertions
 * would still pass, and only the control would go red.
 */

static void test_work_after_idle_stays_unresolved(void)
{
    printf("\n### 1d. work after an intra-execution idle: no stale close ###\n");
    /* Row opens at +0, idles at +10 (COPY FROM STDIN waiting on its client),
     * leaves idle at +30, and is still emitting work at +60 and +90 when the
     * stream ends. No EXEC_END, no exit. */
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        wait_rec(4242, BASE + 10 * MS, 10 * MS, CPU_EVENT, CLIENT_READ, 77),
        wait_rec(4242, BASE + 30 * MS, 20 * MS, CLIENT_READ, IO_READ, 77),
        wait_rec(4242, BASE + 60 * MS, 30 * MS, IO_READ, CPU_EVENT, 77),
        wait_rec(4242, BASE + 90 * MS, 30 * MS, CPU_EVENT, IO_READ, 77),
    };
    struct pgwt_executions_result r;
    run(evs, 5, &r);
    check(r.num_rows == 1, "the EXEC_START is a row (got %d)", r.num_rows);
    if (r.num_rows == 1) {
        const struct pgwt_execution *row = &r.rows[0];
        check(row->in_progress == 1,
              "a 90ms-and-counting execution stays IN PROGRESS, not closed "
              "at its +10ms idle (in_progress=%d end=+%llu ms)",
              row->in_progress,
              (unsigned long long)((row->end_ns - BASE) / MS));
        check(row->end_inferred == 0,
              "and carries no inferred end (end_inferred=%d)",
              row->end_inferred);
    }
    free(r.rows);

    /* CONTROL: identical up to the idle, then the stream simply ends. This
     * one MUST close — it is the cancel shape. A dead close path fails
     * here, so the assertion above cannot pass for the wrong reason. */
    struct pgwt_trace_event ctl[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        wait_rec(4242, BASE + 10 * MS, 10 * MS, CPU_EVENT, CLIENT_READ, 77),
    };
    struct pgwt_executions_result c;
    run(ctl, 2, &c);
    check(c.num_rows == 1 && !c.rows[0].in_progress &&
          c.rows[0].end_ns == BASE + 10 * MS && c.rows[0].end_inferred == 1,
          "CONTROL — the same stream WITHOUT the post-idle work still "
          "closes at +10ms, inferred (the close path is alive)");
    free(c.rows);
}

static void test_one_record_of_work_already_invalidates(void)
{
    printf("\n### 1d'. a SINGLE post-idle record is enough to invalidate ###\n");
    /* Pins the clearing rule to the record that ENTERS a non-idle wait,
     * rather than one record later. With a one-record lag this stream
     * would close at +10 — a boundary the very next record contradicts. */
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        wait_rec(4242, BASE + 10 * MS, 10 * MS, CPU_EVENT, CLIENT_READ, 77),
        wait_rec(4242, BASE + 30 * MS, 20 * MS, CLIENT_READ, IO_READ, 77),
    };
    struct pgwt_executions_result r;
    run(evs, 3, &r);
    check(r.num_rows == 1 && r.rows[0].in_progress == 1,
          "one record leaving idle for IO already blocks the stale close "
          "(in_progress=%d)", r.num_rows == 1 ? r.rows[0].in_progress : -1);
    free(r.rows);
}

static void test_exit_after_work_closes_at_exit(void)
{
    printf("\n### 1e. exit after post-idle work closes at the EXIT ###\n");
    /* Same shape, but the backend exits at +500. The row must close — the
     * backend is gone — at the exit's own timestamp, not at the stale
     * +10ms idle. The exit is the later and still-supported evidence. */
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        wait_rec(4242, BASE + 10 * MS, 10 * MS, CPU_EVENT, CLIENT_READ, 77),
        wait_rec(4242, BASE + 30 * MS, 20 * MS, CLIENT_READ, IO_READ, 77),
        wait_rec(4242, BASE + 60 * MS, 30 * MS, IO_READ, CPU_EVENT, 77),
        wait_rec(4242, BASE + 90 * MS, 30 * MS, CPU_EVENT, IO_READ, 77),
        exit_rec(4242, BASE + 500 * MS, IO_READ, 77),
    };
    struct pgwt_executions_result r;
    run(evs, 6, &r);
    check(r.num_rows == 1, "the EXEC_START is a row (got %d)", r.num_rows);
    if (r.num_rows == 1) {
        const struct pgwt_execution *row = &r.rows[0];
        check(!row->in_progress, "the exit closes the row");
        check(row->end_ns == BASE + 500 * MS,
              "closed at the EXIT (+500 ms), not the stale idle "
              "(got +%llu ms)",
              (unsigned long long)((row->end_ns - BASE) / MS));
        check(row->end_inferred == 1, "still labelled end_inferred");
    }
    free(r.rows);

    /* CONTROL: exit with the pid STILL idle — the idle is the better,
     * earlier boundary and must win over the exit. */
    struct pgwt_trace_event ctl[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        wait_rec(4242, BASE + 10 * MS, 10 * MS, CPU_EVENT, CLIENT_READ, 77),
        exit_rec(4242, BASE + 500 * MS, CLIENT_READ, 77),
    };
    struct pgwt_executions_result c;
    run(ctl, 3, &c);
    check(c.num_rows == 1 && !c.rows[0].in_progress &&
          c.rows[0].end_ns == BASE + 10 * MS,
          "CONTROL — exiting while STILL idle closes at the idle (+10 ms), "
          "not at the exit");
    free(c.rows);
}

static void test_work_after_idle_then_new_statement(void)
{
    printf("\n### 1f. a later EXEC_START still settles the earlier row ###\n");
    /* The EXEC_START path is deliberately NOT gated on the candidate still
     * being current: a second execution proves the first one ended no later
     * than the idle observed while it was open, whatever happened in
     * between (leaving idle, a utility statement, parse/bind work). */
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        wait_rec(4242, BASE + 20 * MS, 20 * MS, CPU_EVENT, CLIENT_READ, 77),
        wait_rec(4242, BASE + 60 * MS, 40 * MS, CLIENT_READ, IO_READ, 77),
        wait_rec(4242, BASE + 70 * MS, 10 * MS, IO_READ, CPU_EVENT, 77),
        mark(4242, BASE + 75 * MS, PGWT_MARKER_EXEC_START, 88),
        mark(4242, BASE + 95 * MS, PGWT_MARKER_EXEC_END, 88),
    };
    struct pgwt_executions_result r;
    run(evs, 6, &r);
    const struct pgwt_execution *a = row_at(&r, BASE);
    check(a && !a->in_progress && a->end_inferred == 1 &&
          a->end_ns == BASE + 20 * MS,
          "the earlier row still closes at its idle (+20 ms) even though "
          "the pid did non-idle work before the next EXEC_START");
    free(r.rows);
}

static void test_sample_records_do_not_invalidate(void)
{
    printf("\n### 1g. SAMPLE records do not invalidate the candidate ###\n");
    /* Deliberate, documented boundary: sampled observations are excluded
     * from execution accounting everywhere in this function, so they
     * neither close a row nor contradict a boundary. Pinned so that a
     * future change to the sample path is a visible decision. */
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        wait_rec(4242, BASE + 10 * MS, 10 * MS, CPU_EVENT, CLIENT_READ, 77),
        sample_rec(4242, BASE + 40 * MS, IO_READ, 77),
        sample_rec(4242, BASE + 70 * MS, IO_READ, 77),
    };
    struct pgwt_executions_result r;
    run(evs, 4, &r);
    check(r.num_rows == 1 && !r.rows[0].in_progress &&
          r.rows[0].end_ns == BASE + 10 * MS,
          "sampled non-idle observations leave the +10 ms close standing");
    free(r.rows);
}

/* ── 2. pid reuse ────────────────────────────────────────────── */

static void test_pid_reuse_does_not_cross_close(void)
{
    printf("\n### 2. pid reuse: neither backend may close the other's row ###\n");
    /* pid 4242 errors, sits idle, then EXITS. The pid number is reused by a
     * brand new backend which starts an execution that is still running. */
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        wait_rec(4242, BASE + 20 * MS, 20 * MS, CPU_EVENT, CLIENT_READ, 77),
        exit_rec(4242, BASE + 100 * MS, CLIENT_READ, 77),
        /* ---- same pid number, different backend ---- */
        mark(4242, BASE + 200 * MS, PGWT_MARKER_EXEC_START, 99),
        wait_rec(4242, BASE + 210 * MS, 10 * MS, CPU_EVENT, IO_READ, 99),
    };
    struct pgwt_executions_result r;
    run(evs, 5, &r);
    check(r.num_rows == 2, "two rows, one per backend (got %d)", r.num_rows);
    const struct pgwt_execution *old_row = row_at(&r, BASE);
    const struct pgwt_execution *new_row = row_at(&r, BASE + 200 * MS);
    check(old_row && !old_row->in_progress &&
          old_row->end_ns == BASE + 20 * MS,
          "the OLD row closed at its own idle boundary, before the exit");
    check(new_row && new_row->in_progress,
          "the NEW backend's still-running row was NOT closed by the dead "
          "backend's leftovers");
    free(r.rows);
}

static void test_pid_reuse_new_idle_cannot_close_old(void)
{
    printf("\n### 2b. reuse with no idle before the exit ###\n");
    /* The old backend disconnects mid-query: no idle record at all, just the
     * exit. The reused pid then goes idle. That idle must belong to the NEW
     * backend's row, and the old row must have been closed at the exit. */
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        wait_rec(4242, BASE + 20 * MS, 20 * MS, CPU_EVENT, IO_READ, 77),
        exit_rec(4242, BASE + 40 * MS, IO_READ, 77),
        mark(4242, BASE + 200 * MS, PGWT_MARKER_EXEC_START, 99),
        wait_rec(4242, BASE + 250 * MS, 50 * MS, CPU_EVENT, CLIENT_READ, 99),
    };
    struct pgwt_executions_result r;
    run(evs, 5, &r);
    const struct pgwt_execution *old_row = row_at(&r, BASE);
    const struct pgwt_execution *new_row = row_at(&r, BASE + 200 * MS);
    check(old_row && !old_row->in_progress && old_row->end_inferred == 1 &&
          old_row->end_ns == BASE + 40 * MS,
          "disconnect mid-query closes at the process exit, labelled");
    check(new_row && !new_row->in_progress &&
          new_row->end_ns == BASE + 250 * MS,
          "the new backend's own idle closes the NEW row, at +250ms");
    free(r.rows);
}

static void test_pid_reuse_does_not_inherit_plan(void)
{
    printf("\n### 2c. reuse must not inherit the dead backend's plan ###\n");
    /* The dead backend planned a statement it never got to execute (the
     * connection dropped between PLAN_END and EXEC_START). If the exit does
     * not WIPE its per-pid slot, that ready-but-unconsumed plan is still
     * sitting there when the reused pid number starts its first execution,
     * which silently adopts it: a plan_ms measured by a different backend,
     * before the new one even connected. Closing the orphan row is not
     * enough on its own — the whole slot has to go. */
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        mark(4242, BASE + 10 * MS, PGWT_MARKER_PLAN_START, 0),
        mark(4242, BASE + 15 * MS, PGWT_MARKER_PLAN_END, 0),
        exit_rec(4242, BASE + 20 * MS, CPU_EVENT, 77),
        /* ---- same pid number, different backend, no plan of its own ---- */
        mark(4242, BASE + 200 * MS, PGWT_MARKER_EXEC_START, 99),
        mark(4242, BASE + 210 * MS, PGWT_MARKER_EXEC_END, 99),
    };
    struct pgwt_executions_result r;
    run(evs, 6, &r);
    const struct pgwt_execution *old_row = row_at(&r, BASE);
    const struct pgwt_execution *new_row = row_at(&r, BASE + 200 * MS);
    check(old_row && !old_row->in_progress && old_row->end_inferred == 1 &&
          old_row->end_ns == BASE + 20 * MS,
          "the dead backend's orphan row closed at its process exit");
    check(new_row && !new_row->has_plan && new_row->plan_start_ns == 0,
          "the reused pid's row has NO plan — not the dead backend's");
    free(r.rows);
}

/* ── 3. no idle record at all ────────────────────────────────── */

static void test_absent_idle_stays_unresolved(void)
{
    printf("\n### 3. no idle record, no exit: the row stays unresolved ###\n");
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        wait_rec(4242, BASE + 20 * MS, 20 * MS, CPU_EVENT, IO_READ, 77),
        wait_rec(4242, BASE + 60 * MS, 40 * MS, IO_READ, CLIENT_WRITE, 77),
    };
    struct pgwt_executions_result r;
    run(evs, 3, &r);
    check(r.num_rows == 1, "one row (got %d)", r.num_rows);
    if (r.num_rows == 1) {
        check(r.rows[0].in_progress == 1,
              "in_progress stays 1 — visibly unresolved");
        check(r.rows[0].end_ns == 0,
              "end_ns is 0: no fabricated end (server serializes null)");
        check(r.rows[0].end_inferred == 0,
              "end_inferred stays 0 while in_progress");
    }
    free(r.rows);
}

static void test_end_before_start_is_refused(void)
{
    printf("\n### 3b. a deduced end before the start is refused ###\n");
    /* Defensive: a reordered/corrupt idle record must never produce a
     * negative duration. The row stays unresolved instead. */
    struct pgwt_trace_event evs[] = {
        wait_rec(4242, BASE - 10 * MS, MS, CPU_EVENT, CLIENT_READ, 77),
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
    };
    struct pgwt_executions_result r;
    run(evs, 2, &r);
    check(r.num_rows == 1 && r.rows[0].in_progress == 1 &&
          r.rows[0].end_ns == 0,
          "an idle record that PRECEDES the start closes nothing");
    free(r.rows);
}

/* ── 4. a measured end always wins ───────────────────────────── */

static void test_copy_from_stdin_keeps_measured_end(void)
{
    printf("\n### 4. COPY FROM STDIN: idle INSIDE an execution ###\n");
    /* COPY FROM STDIN waits on Client:ClientRead repeatedly while the
     * execution is genuinely still running, then completes normally. The
     * measured EXEC_END must win; the row must not be truncated at the
     * first client stall, and must not be labelled inferred. */
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        wait_rec(4242, BASE + 10 * MS, 10 * MS, CPU_EVENT, CLIENT_READ, 77),
        wait_rec(4242, BASE + 30 * MS, 20 * MS, CLIENT_READ, CPU_EVENT, 77),
        wait_rec(4242, BASE + 40 * MS, 10 * MS, CPU_EVENT, CLIENT_READ, 77),
        wait_rec(4242, BASE + 90 * MS, 50 * MS, CLIENT_READ, CPU_EVENT, 77),
        mark(4242, BASE + 100 * MS, PGWT_MARKER_EXEC_END, 77),
    };
    struct pgwt_executions_result r;
    run(evs, 6, &r);
    check(r.num_rows == 1, "one row (got %d)", r.num_rows);
    if (r.num_rows == 1) {
        check(!r.rows[0].in_progress && r.rows[0].end_ns == BASE + 100 * MS,
              "closed at the MEASURED EXEC_END (+100ms), not the first "
              "client stall (+10ms)");
        check(r.rows[0].end_inferred == 0,
              "a measured completion is NOT labelled inferred");
    }
    free(r.rows);
}

static void test_completed_row_with_trailing_idle(void)
{
    printf("\n### 4b. a normally-completed row is untouched ###\n");
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        wait_rec(4242, BASE + 20 * MS, 20 * MS, CPU_EVENT, IO_READ, 77),
        mark(4242, BASE + 30 * MS, PGWT_MARKER_EXEC_END, 77),
        wait_rec(4242, BASE + 31 * MS, MS, CPU_EVENT, CLIENT_READ, 77),
        exit_rec(4242, BASE + 500 * MS, CLIENT_READ, 77),
    };
    struct pgwt_executions_result r;
    run(evs, 5, &r);
    check(r.num_rows == 1 && !r.rows[0].in_progress &&
          r.rows[0].end_ns == BASE + 30 * MS && r.rows[0].end_inferred == 0,
          "trailing idle and exit do not move or relabel a measured end");
    free(r.rows);
}

/* ── 5. bypass suite: every way this could fail to see ───────── */

static void test_bypass_empty_input(void)
{
    printf("\n### 5a. empty event stream ###\n");
    struct pgwt_executions_result r;
    run(NULL, 0, &r);
    check(!r.failed && r.num_rows == 0,
          "no events -> no rows, no crash, not 'failed'");
    free(r.rows);
}

static void test_bypass_no_exec_start(void)
{
    printf("\n### 5b. the thing being checked is ABSENT, not wrong ###\n");
    /* Idle records and an exit for a pid that never started an execution:
     * the close path must not manufacture a row out of them. */
    struct pgwt_trace_event evs[] = {
        wait_rec(4242, BASE + 10 * MS, 10 * MS, CPU_EVENT, CLIENT_READ, 77),
        wait_rec(4242, BASE + 30 * MS, 20 * MS, CLIENT_READ, CPU_EVENT, 77),
        exit_rec(4242, BASE + 40 * MS, CPU_EVENT, 77),
    };
    struct pgwt_executions_result r;
    run(evs, 3, &r);
    check(!r.failed && r.num_rows == 0,
          "idle records with no EXEC_START invent no row (got %d)",
          r.num_rows);
    free(r.rows);
}

static void test_bypass_non_idle_client_event(void)
{
    printf("\n### 5c. a NON-idle wait must not satisfy the close ###\n");
    /* Client:ClientWrite is a Client-class wait that is NOT idle: a backend
     * streaming a large result set is still executing. Closing there would
     * truncate every big SELECT. */
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        wait_rec(4242, BASE + 20 * MS, 20 * MS, CPU_EVENT, CLIENT_WRITE, 77),
        wait_rec(4242, BASE + 60 * MS, 40 * MS, CLIENT_WRITE, CPU_EVENT, 77),
    };
    struct pgwt_executions_result r;
    run(evs, 3, &r);
    check(r.num_rows == 1 && r.rows[0].in_progress == 1,
          "ClientWrite does not close a running execution");
    free(r.rows);
}

static void test_bypass_sample_record(void)
{
    printf("\n### 5d. a SAMPLE is not a transition ###\n");
    /* A sampled record says "idle at this instant", not "went idle at this
     * timestamp"; its ts is the sampler's tick, not a boundary. Closing on
     * one would date the end by up to a whole sample period. */
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        sample_rec(4242, BASE + 20 * MS, CLIENT_READ, 77),
        sample_rec(4242, BASE + 40 * MS, CLIENT_READ, 77),
    };
    struct pgwt_executions_result r;
    run(evs, 3, &r);
    check(r.num_rows == 1 && r.rows[0].in_progress == 1,
          "sampled idle readings do not close an exact row");
    free(r.rows);
}

static void test_bypass_exit_with_no_open_row(void)
{
    printf("\n### 5e. exit / idle with nothing open ###\n");
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        mark(4242, BASE + 10 * MS, PGWT_MARKER_EXEC_END, 77),
        wait_rec(4242, BASE + 20 * MS, MS, CPU_EVENT, CLIENT_READ, 77),
        exit_rec(4242, BASE + 30 * MS, CLIENT_READ, 77),
        exit_rec(4242, BASE + 40 * MS, CPU_EVENT, 77),
    };
    struct pgwt_executions_result r;
    run(evs, 5, &r);
    check(!r.failed && r.num_rows == 1 && r.rows[0].end_ns == BASE + 10 * MS,
          "a closed row is not reopened or re-ended by later exits");
    free(r.rows);
}

static void test_bypass_other_pid_row(void)
{
    printf("\n### 5f. one pid's idle must not close another pid's row ###\n");
    struct pgwt_trace_event evs[] = {
        mark(4242, BASE, PGWT_MARKER_EXEC_START, 77),
        mark(5353, BASE + 5 * MS, PGWT_MARKER_EXEC_START, 88),
        /* only pid 5353 goes idle */
        wait_rec(5353, BASE + 50 * MS, 45 * MS, CPU_EVENT, CLIENT_READ, 88),
        wait_rec(4242, BASE + 60 * MS, 60 * MS, CPU_EVENT, IO_READ, 77),
    };
    struct pgwt_executions_result r;
    run(evs, 4, &r);
    const struct pgwt_execution *a = row_at(&r, BASE);
    const struct pgwt_execution *b = row_at(&r, BASE + 5 * MS);
    check(a && a->in_progress == 1 && a->pid == 4242,
          "pid 4242's row is untouched by pid 5353's idle");
    check(b && !b->in_progress && b->end_ns == BASE + 50 * MS,
          "pid 5353's own row closed at its own idle");
    free(r.rows);
}

/* ── main ────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== test_compute_orphan_close ===\n");

    test_cancel_closes_at_idle();
    test_close_is_not_elapsed_so_far();
    test_next_statement_settles_previous();
    test_work_after_idle_stays_unresolved();
    test_one_record_of_work_already_invalidates();
    test_exit_after_work_closes_at_exit();
    test_work_after_idle_then_new_statement();
    test_sample_records_do_not_invalidate();
    test_pid_reuse_does_not_cross_close();
    test_pid_reuse_new_idle_cannot_close_old();
    test_pid_reuse_does_not_inherit_plan();
    test_absent_idle_stays_unresolved();
    test_end_before_start_is_refused();
    test_copy_from_stdin_keeps_measured_end();
    test_completed_row_with_trailing_idle();
    test_bypass_empty_input();
    test_bypass_no_exec_start();
    test_bypass_non_idle_client_event();
    test_bypass_sample_record();
    test_bypass_exit_with_no_open_row();
    test_bypass_other_pid_row();

    printf("\n=== %d checks, %d failed ===\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
