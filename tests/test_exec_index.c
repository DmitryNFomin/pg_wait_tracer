/* test_exec_index.c — the executions index (paint-latency plan, Phase 3).
 *
 * WHAT IS BEING PINNED
 * --------------------
 * src/exec_index.c claims that one window query over an append-only index
 * returns BIT-EXACTLY the boundary fields pgwt_compute_executions() returns
 * for the same window, while examining O(executions in window) index rows
 * instead of O(events in window). Both halves of that are checked here: the
 * agreement, field by field with no epsilon anywhere, and the work actually
 * done, counted.
 *
 * THE ORACLE IS THE REAL FUNCTION. pgwt_compute_executions (src/compute.c)
 * is linked in and called; exec_index.c is an independent reimplementation
 * of the marker walk, so a mistake in either side shows up as a
 * disagreement. Nothing is shared between them but the struct.
 *
 * THE ORACLE'S INPUT IS THE SERVER'S INPUT. src/server.c
 * load_execution_rows() hands pgwt_compute_executions a markers-only prefix
 * of everything below `from`, concatenated with every event in [from, to].
 * oracle_rows() below builds exactly that array, so agreement here is
 * agreement with what the product computes, not with a convenient
 * simplification.
 *
 * WHAT MAKES THIS PASS WHILE BROKEN — and what stops it
 * -----------------------------------------------------
 * 1. Feeding every fixture as ONE block. The carry-over across block
 *    boundaries — the entire point of Phase 3 — is then never exercised,
 *    and every bug in the flush/restore path is invisible. Stopped by
 *    sweep A: EVERY cut set of each fixture (2^(n-1), capped), plus
 *    counters that must prove at least one execution actually spanned a cut
 *    and at least one spanned three or more blocks.
 * 2. Vacuity. "index == oracle" is satisfied by 0 == 0, which is how a
 *    whole suite passes against an index that returns nothing. Stopped by
 *    counters: rows actually compared, in-progress rows, rows that started
 *    before the window, CMD_END-inferred rows, plan-bearing rows and
 *    prefilter skips are each asserted > 0 by name. A fixture with no
 *    markers is included on purpose and deliberately does NOT count
 *    towards them.
 * 3. A comparator that cannot see. Stopped by the mutation probes: each of
 *    the ten boundary fields is perturbed by the smallest possible amount
 *    (one nanosecond, one count, one flag) on a real result and the
 *    comparator must report that exact field.
 * 4. A gate that cannot establish an answer but approves anyway. Stopped by
 *    the bypass suite: an empty index, an unsealed index, a wrong version,
 *    a skipped block, a reordered block, an out-of-order record, a record
 *    outside its block's bounds, a window outside coverage, an inverted
 *    window, a NULL index and an injected allocation failure at every
 *    injection point must each REFUSE with their own reason and produce no
 *    rows. "No index" must never read as "no executions".
 *
 * WHAT DEPENDS ON TIMING OR ORDERING. Nothing wall-clock: every fixture is
 * a literal array and every expectation is a literal constant or the
 * oracle's own answer. Event ORDER matters, and is pinned by the index
 * refusing an out-of-order feed rather than silently reordering. The one
 * timing number printed (index query vs raw recompute) is REPORTED, never
 * asserted.
 *
 * Pure: links ../src/exec_index.c + ../src/compute.c. No daemon, no
 * PostgreSQL, no root, no files.
 */
#include "exec_index.h"
#include "compute.h"
#include "pg_wait_tracer.h"
#include "summary_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, fmt, ...) do {                                      \
    tests_run++;                                                        \
    if (cond) { printf("  PASS: " fmt "\n", ##__VA_ARGS__); }           \
    else { tests_failed++; printf("  FAIL(%d): " fmt "\n", __LINE__,    \
                                  ##__VA_ARGS__); }                     \
} while (0)

/* compute.c's only foreign symbol. */
int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns;
    (void)visitor; (void)ctx;
    return -1;
}

#define MS   1000000ULL
#define BASE 1000000000000ULL
#define WE_IO ((0x0AU << 24) | 21U)

/* ── fixtures ─────────────────────────────────────────────── */

#define MAXEV 24

struct fixture {
    const char *name;
    struct pgwt_trace_event ev[MAXEV];
    int n;
};

static struct pgwt_trace_event mk(uint64_t ts, uint32_t pid, uint32_t kind,
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

static struct pgwt_trace_event wt(uint64_t ts, uint32_t pid, uint64_t dur,
                                  uint64_t qid)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;
    e.pid = pid;
    e.old_event = WE_IO;
    e.new_event = 0;
    e.duration_ns = dur;
    e.query_id = qid;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return e;
}

static void fx_add(struct fixture *f, struct pgwt_trace_event e)
{
    if (f->n >= MAXEV) { fprintf(stderr, "fixture overflow\n"); exit(2); }
    f->ev[f->n++] = e;
}

#define P1 4001U
#define P2 4002U
#define PW 4003U   /* a parallel worker: waits, never markers */

static struct fixture g_fx[16];
static int g_nfx = 0;

static struct fixture *new_fx(const char *name)
{
    struct fixture *f = &g_fx[g_nfx++];
    memset(f, 0, sizeof(*f));
    f->name = name;
    return f;
}

static void build_fixtures(void)
{
    struct fixture *f;

    /* F1 plain: plan, exec, one wait, real EXEC_END, CMD_END. */
    f = new_fx("plain plan+exec+end");
    fx_add(f, mk(BASE + 10 * MS, P1, PGWT_MARKER_CMD_START, 7));
    fx_add(f, mk(BASE + 20 * MS, P1, PGWT_MARKER_PLAN_START, 7));
    fx_add(f, mk(BASE + 30 * MS, P1, PGWT_MARKER_PLAN_END, 7));
    fx_add(f, mk(BASE + 40 * MS, P1, PGWT_MARKER_EXEC_START, 7));
    fx_add(f, wt(BASE + 60 * MS, P1, 15 * MS, 7));
    fx_add(f, mk(BASE + 80 * MS, P1, PGWT_MARKER_EXEC_END, 7));
    fx_add(f, mk(BASE + 90 * MS, P1, PGWT_MARKER_CMD_END, 7));

    /* F2 open at capture end: no EXEC_END, no CMD_END, ever. Every window
     * must report it in_progress with end_ns 0 — the long query that must
     * not vanish. */
    f = new_fx("open at capture end");
    fx_add(f, mk(BASE + 10 * MS, P1, PGWT_MARKER_EXEC_START, 11));
    fx_add(f, wt(BASE + 30 * MS, P1, 10 * MS, 11));
    fx_add(f, wt(BASE + 50 * MS, P1, 10 * MS, 11));
    fx_add(f, wt(BASE + 70 * MS, P1, 10 * MS, 11));

    /* F3 spans many blocks: one execution with six intervening waits, so a
     * cut sweep puts its start and end three or more blocks apart. */
    f = new_fx("one execution across many blocks");
    fx_add(f, mk(BASE + 10 * MS, P1, PGWT_MARKER_EXEC_START, 21));
    fx_add(f, wt(BASE + 20 * MS, P1, 5 * MS, 21));
    fx_add(f, wt(BASE + 30 * MS, P1, 5 * MS, 21));
    fx_add(f, wt(BASE + 40 * MS, P1, 5 * MS, 21));
    fx_add(f, wt(BASE + 50 * MS, P1, 5 * MS, 21));
    fx_add(f, wt(BASE + 60 * MS, P1, 5 * MS, 21));
    fx_add(f, mk(BASE + 70 * MS, P1, PGWT_MARKER_EXEC_END, 21));

    /* F4 the #222 shape: a second EXEC_START with the first still open, one
     * EXEC_END (which closes the INNER row), then CMD_END closing the
     * orphan with end_inferred. A single active_row loses the outer row
     * here; a plain CMD_END cannot close it. */
    f = new_fx("#222: two starts, one end, CMD_END closes the orphan");
    fx_add(f, mk(BASE + 10 * MS, P1, PGWT_MARKER_EXEC_START, 31));
    fx_add(f, wt(BASE + 15 * MS, P1, 2 * MS, 31));
    fx_add(f, mk(BASE + 20 * MS, P1, PGWT_MARKER_EXEC_START, 32));
    fx_add(f, wt(BASE + 25 * MS, P1, 2 * MS, 32));
    fx_add(f, mk(BASE + 30 * MS, P1, PGWT_MARKER_EXEC_END, 32));
    fx_add(f, wt(BASE + 35 * MS, P1, 2 * MS, 32));
    fx_add(f, mk(BASE + 40 * MS, P1, PGWT_MARKER_CMD_END, 32));

    /* F5 concurrent pids plus a parallel worker that emits only waits. The
     * worker contributes nothing to any boundary field, which is the
     * MARKERS ONLY claim under adversarial input. */
    f = new_fx("two concurrent pids + a marker-less parallel worker");
    fx_add(f, mk(BASE + 10 * MS, P1, PGWT_MARKER_EXEC_START, 41));
    fx_add(f, mk(BASE + 15 * MS, P2, PGWT_MARKER_EXEC_START, 42));
    fx_add(f, wt(BASE + 20 * MS, PW, 5 * MS, 41));
    fx_add(f, wt(BASE + 25 * MS, P2, 5 * MS, 42));
    fx_add(f, mk(BASE + 30 * MS, P2, PGWT_MARKER_EXEC_END, 42));
    fx_add(f, wt(BASE + 35 * MS, PW, 5 * MS, 41));
    fx_add(f, mk(BASE + 40 * MS, P1, PGWT_MARKER_EXEC_END, 41));
    fx_add(f, mk(BASE + 50 * MS, P1, PGWT_MARKER_CMD_END, 41));

    /* F6 unbalanced the other way: an EXEC_END with nothing open (the pop
     * returns -1), then a normal pair. */
    f = new_fx("EXEC_END with an empty stack");
    fx_add(f, mk(BASE + 10 * MS, P1, PGWT_MARKER_EXEC_END, 51));
    fx_add(f, mk(BASE + 20 * MS, P1, PGWT_MARKER_EXEC_START, 52));
    fx_add(f, wt(BASE + 25 * MS, P1, 2 * MS, 52));
    fx_add(f, mk(BASE + 30 * MS, P1, PGWT_MARKER_EXEC_END, 52));

    /* F7 zero-duration execution: EXEC_START and EXEC_END on the same
     * nanosecond. The reference's `ts >= start_ns` guard is satisfied at
     * equality, so the row closes with end_ns == start_ns. */
    f = new_fx("zero-length execution (start == end)");
    fx_add(f, mk(BASE + 10 * MS, P1, PGWT_MARKER_EXEC_START, 61));
    fx_add(f, mk(BASE + 10 * MS, P1, PGWT_MARKER_EXEC_END, 61));
    fx_add(f, wt(BASE + 20 * MS, P1, 2 * MS, 61));

    /* F8 a plan CMD_END throws away, then an execution with no plan. */
    f = new_fx("CMD_END discards a ready plan");
    fx_add(f, mk(BASE + 10 * MS, P1, PGWT_MARKER_PLAN_START, 71));
    fx_add(f, mk(BASE + 20 * MS, P1, PGWT_MARKER_PLAN_END, 71));
    fx_add(f, mk(BASE + 30 * MS, P1, PGWT_MARKER_CMD_END, 71));
    fx_add(f, mk(BASE + 40 * MS, P1, PGWT_MARKER_EXEC_START, 71));
    fx_add(f, mk(BASE + 50 * MS, P1, PGWT_MARKER_EXEC_END, 71));

    /* F9 plan/exec query-id mismatch: has_plan must stay 0. */
    f = new_fx("plan query_id mismatch");
    fx_add(f, mk(BASE + 10 * MS, P1, PGWT_MARKER_PLAN_START, 81));
    fx_add(f, mk(BASE + 20 * MS, P1, PGWT_MARKER_PLAN_END, 81));
    fx_add(f, mk(BASE + 30 * MS, P1, PGWT_MARKER_EXEC_START, 82));
    fx_add(f, mk(BASE + 40 * MS, P1, PGWT_MARKER_EXEC_END, 82));

    /* F10 query_id arrives only on EXEC_END. A window that stops before
     * that marker must report query_id 0, exactly as the reference does
     * when the marker is not in its array. */
    f = new_fx("query_id filled by EXEC_END only");
    fx_add(f, mk(BASE + 10 * MS, P1, PGWT_MARKER_EXEC_START, 0));
    fx_add(f, wt(BASE + 20 * MS, P1, 2 * MS, 0));
    fx_add(f, mk(BASE + 30 * MS, P1, PGWT_MARKER_EXEC_END, 91));
    fx_add(f, wt(BASE + 40 * MS, P1, 2 * MS, 91));

    /* F11 three open rows closed by one CMD_END, all inferred. */
    f = new_fx("three orphans closed by one CMD_END");
    fx_add(f, mk(BASE + 10 * MS, P1, PGWT_MARKER_EXEC_START, 101));
    fx_add(f, mk(BASE + 20 * MS, P1, PGWT_MARKER_EXEC_START, 102));
    fx_add(f, mk(BASE + 30 * MS, P1, PGWT_MARKER_EXEC_START, 103));
    fx_add(f, wt(BASE + 35 * MS, P1, 2 * MS, 103));
    fx_add(f, mk(BASE + 40 * MS, P1, PGWT_MARKER_CMD_END, 103));
    fx_add(f, wt(BASE + 50 * MS, P1, 2 * MS, 103));

    /* F12 markers absent, not wrong: waits only. Both sides must say "no
     * executions" WITHOUT refusing, and this fixture must not be allowed to
     * satisfy any non-vacuity counter. */
    f = new_fx("no markers at all (absent, not wrong)");
    fx_add(f, wt(BASE + 10 * MS, P1, 2 * MS, 111));
    fx_add(f, wt(BASE + 20 * MS, P1, 2 * MS, 111));
    fx_add(f, wt(BASE + 30 * MS, P2, 2 * MS, 112));

    /* F13 escalation and CMD_START markers interleaved: inert for
     * boundaries, so they must change nothing. */
    f = new_fx("inert markers interleaved");
    fx_add(f, mk(BASE + 5 * MS, 0, PGWT_MARKER_ESCALATE_START, 0));
    fx_add(f, mk(BASE + 10 * MS, P1, PGWT_MARKER_CMD_START, 121));
    fx_add(f, mk(BASE + 20 * MS, P1, PGWT_MARKER_EXEC_START, 121));
    fx_add(f, mk(BASE + 25 * MS, 0, PGWT_MARKER_ESCALATE_END, 0));
    fx_add(f, mk(BASE + 30 * MS, P1, PGWT_MARKER_EXEC_END, 121));
}

/* ── the oracle, in the server's own input shape ──────────── */

static void oracle_rows(const struct fixture *f, uint64_t from, uint64_t to,
                        struct pgwt_executions_result *out)
{
    struct pgwt_trace_event arr[MAXEV * 2];
    int n = 0;
    /* server_load_exec_prefix(): markers only, everything below `from`. */
    for (int i = 0; i < f->n; i++)
        if (f->ev[i].timestamp_ns < from &&
            PGWT_IS_MARKER(f->ev[i].old_event))
            arr[n++] = f->ev[i];
    /* server_load_events_fi(): every record in [from, to]. */
    for (int i = 0; i < f->n; i++)
        if (f->ev[i].timestamp_ns >= from && f->ev[i].timestamp_ns <= to)
            arr[n++] = f->ev[i];
    pgwt_compute_executions(arr, n, from, to, NULL, NULL, 0, out);
}

/* ── the comparator ───────────────────────────────────────── */

static const char *g_field_names[] = {
    "pid", "query_id", "start_ns", "end_ns", "plan_start_ns", "plan_end_ns",
    "has_plan", "in_progress", "end_inferred", "started_before_window",
};
#define N_BOUNDARY_FIELDS 10

/* Bit-exact, no epsilon. Returns the index of the first differing boundary
 * field, or -1 when every one of them is identical. */
static int exec_boundary_diff(const struct pgwt_execution *a,
                              const struct pgwt_execution *b)
{
    if (a->pid != b->pid) return 0;
    if (a->query_id != b->query_id) return 1;
    if (a->start_ns != b->start_ns) return 2;
    if (a->end_ns != b->end_ns) return 3;
    if (a->plan_start_ns != b->plan_start_ns) return 4;
    if (a->plan_end_ns != b->plan_end_ns) return 5;
    if (a->has_plan != b->has_plan) return 6;
    if (a->in_progress != b->in_progress) return 7;
    if (a->end_inferred != b->end_inferred) return 8;
    if (a->started_before_window != b->started_before_window) return 9;
    return -1;
}

/* ── index construction over an arbitrary cut set ─────────── */

/* Bit i of `cuts` (i in 0..n-2) means "start a new block before event i+1".
 * Pieces are non-empty by construction, so every block's bounds are its own
 * first/last record timestamp — the shape a real committed block has. */
static int build_index(const struct fixture *f, unsigned cuts,
                       struct pgwt_exec_index *idx, int *n_blocks_out)
{
    pgwt_exec_index_init(idx);
    int blk = 0, s = 0;
    for (int e = 1; e <= f->n; e++) {
        int cut = (e == f->n) || ((cuts >> (e - 1)) & 1u);
        if (!cut)
            continue;
        if (pgwt_exec_index_add_block(idx, blk, f->ev[s].timestamp_ns,
                                      f->ev[e - 1].timestamp_ns,
                                      &f->ev[s], e - s) != 0)
            return -1;
        blk++;
        s = e;
    }
    if (n_blocks_out)
        *n_blocks_out = blk;
    return pgwt_exec_index_seal(idx);
}

/* How many blocks an execution's [start, close] spans under this cut set.
 * Counted from the index itself so the number reported is the number the
 * carry-over actually had to bridge. */
static int span_blocks(const struct pgwt_exec_index *idx, int row)
{
    const struct pgwt_exec_index_entry *e = &idx->rows[row];
    uint64_t lo = e->start_ns;
    uint64_t hi = e->closed ? e->close_ns : idx->cover_to_ns;
    int n = 0;
    for (int c = 0; c < idx->n_chunks; c++)
        if (idx->chunks[c].block_first_ns <= hi &&
            idx->chunks[c].block_last_ns >= lo)
            n++;
    return n;
}

/* ── non-vacuity counters ─────────────────────────────────── */

static long c_triples, c_rows, c_crossing, c_span3, c_inprog, c_before,
           c_inferred, c_plan, c_skipped, c_splits,
           c_zero_len, c_multipid;
static long c_mismatch;
static char g_first_mismatch[512];

static void note_mismatch(const struct fixture *f, unsigned cuts,
                          uint64_t from, uint64_t to, int row, int field,
                          const struct pgwt_execution *a,
                          const struct pgwt_execution *b)
{
    c_mismatch++;
    if (g_first_mismatch[0])
        return;
    snprintf(g_first_mismatch, sizeof(g_first_mismatch),
             "%s cuts=0x%x window=[+%llu,+%llu] row=%d field=%s "
             "index={pid=%u qid=%llu s=+%llu e=+%llu plan=%d ip=%d inf=%d "
             "sbw=%d} oracle={pid=%u qid=%llu s=+%llu e=+%llu plan=%d ip=%d "
             "inf=%d sbw=%d}",
             f->name, cuts,
             (unsigned long long)(from - BASE) / MS,
             (unsigned long long)(to - BASE) / MS, row,
             field >= 0 ? g_field_names[field] : "row-count",
             a->pid, (unsigned long long)a->query_id,
             (unsigned long long)(a->start_ns - BASE) / MS,
             (unsigned long long)(a->end_ns ? (a->end_ns - BASE) / MS : 0),
             a->has_plan, a->in_progress, a->end_inferred,
             a->started_before_window,
             b->pid, (unsigned long long)b->query_id,
             (unsigned long long)(b->start_ns - BASE) / MS,
             (unsigned long long)(b->end_ns ? (b->end_ns - BASE) / MS : 0),
             b->has_plan, b->in_progress, b->end_inferred,
             b->started_before_window);
}

/* One (fixture, cut set, window) comparison. Returns 0 when the index and
 * the oracle agree bit-exactly, -1 otherwise. A refusal is a failure here:
 * every window these sweeps generate is inside coverage by construction. */
static int compare_one(const struct fixture *f, unsigned cuts,
                       uint64_t from, uint64_t to, int vacuous_ok)
{
    struct pgwt_exec_index idx;
    int n_blocks = 0;
    if (build_index(f, cuts, &idx, &n_blocks) != 0) {
        printf("  FAIL: build refused for %s cuts=0x%x (%s)\n", f->name, cuts,
               pgwt_exec_index_refusal_str(idx.build_refusal));
        pgwt_exec_index_free(&idx);
        return -1;
    }

    struct pgwt_exec_index_query_result q;
    int rc = pgwt_exec_index_query(&idx, from, to, &q);
    if (rc != 0) {
        printf("  FAIL: query refused for %s cuts=0x%x window=[+%llu,+%llu]"
               " (%s)\n", f->name, cuts,
               (unsigned long long)(from - BASE) / MS,
               (unsigned long long)(to - BASE) / MS,
               pgwt_exec_index_refusal_str(q.refused));
        pgwt_exec_index_free(&idx);
        return -1;
    }

    struct pgwt_executions_result ref;
    oracle_rows(f, from, to, &ref);
    if (ref.failed) {
        printf("  FAIL: ORACLE failed (allocation) for %s\n", f->name);
        free(ref.rows);
        pgwt_exec_index_query_free(&q);
        pgwt_exec_index_free(&idx);
        return -1;
    }

    int bad = 0;
    if (q.num_rows != ref.num_rows) {
        struct pgwt_execution zero;
        memset(&zero, 0, sizeof(zero));
        note_mismatch(f, cuts, from, to, -1, -1,
                      q.num_rows ? &q.rows[0] : &zero,
                      ref.num_rows ? &ref.rows[0] : &zero);
        bad = 1;
    } else {
        for (int i = 0; i < q.num_rows; i++) {
            int d = exec_boundary_diff(&q.rows[i], &ref.rows[i]);
            if (d >= 0) {
                note_mismatch(f, cuts, from, to, i, d, &q.rows[i],
                              &ref.rows[i]);
                bad = 1;
            }
        }
    }

    /* Prefilter safety: the chunk skip must never change the row set. A
     * brute-force scan over EVERY index row with the same retention
     * predicate has to produce the identical answer. */
    if (!bad) {
        int brute = 0;
        for (int i = 0; i < idx.n_rows; i++) {
            const struct pgwt_exec_index_entry *e = &idx.rows[i];
            int ip = (!e->closed || e->close_ns > to);
            uint64_t end = ip ? 0 : e->close_ns;
            if (pgwt_exec_index_retains(e->start_ns, end, ip, from, to))
                brute++;
        }
        if (brute != q.num_rows) {
            printf("  FAIL: prefilter dropped rows for %s cuts=0x%x: "
                   "prefiltered=%d brute=%d\n", f->name, cuts, q.num_rows,
                   brute);
            bad = 1;
        }
    }

    /* Counters — only from fixtures that can legitimately feed them. */
    c_triples++;
    c_skipped += q.chunks_total - q.chunks_scanned;
    if (!vacuous_ok) {
        c_rows += q.num_rows;
        uint32_t seen_pid = 0;
        int distinct = 0;
        for (int i = 0; i < q.num_rows; i++) {
            if (q.rows[i].in_progress) c_inprog++;
            if (q.rows[i].started_before_window) c_before++;
            if (q.rows[i].end_inferred) c_inferred++;
            if (q.rows[i].has_plan) c_plan++;
            if (!q.rows[i].in_progress &&
                q.rows[i].end_ns == q.rows[i].start_ns) c_zero_len++;
            if (i == 0) { seen_pid = q.rows[i].pid; distinct = 1; }
            else if (q.rows[i].pid != seen_pid) distinct = 2;
        }
        if (distinct == 2) c_multipid++;
        for (int i = 0; i < idx.n_rows; i++) {
            int sp = span_blocks(&idx, i);
            if (sp >= 2) c_crossing++;
            if (sp >= 3) c_span3++;
        }
    }

    free(ref.rows);
    pgwt_exec_index_query_free(&q);
    pgwt_exec_index_free(&idx);
    return bad ? -1 : 0;
}

/* Distinct timestamps of a fixture, ascending. */
static int fx_times(const struct fixture *f, uint64_t *out)
{
    int n = 0;
    for (int i = 0; i < f->n; i++) {
        int dup = 0;
        for (int j = 0; j < n; j++)
            if (out[j] == f->ev[i].timestamp_ns) dup = 1;
        if (!dup) out[n++] = f->ev[i].timestamp_ns;
    }
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (out[j] < out[i]) { uint64_t t = out[i]; out[i] = out[j]; out[j] = t; }
    return n;
}

/* ── sweep A: every cut set x window pairs from the fixture's own times ── */

#define MAX_SPLITS 4096

static void sweep_splits(void)
{
    printf("--- sweep A: EVERY block split of every fixture ---\n");
    long bad = 0;
    for (int k = 0; k < g_nfx; k++) {
        const struct fixture *f = &g_fx[k];
        int vac = (strstr(f->name, "no markers") != NULL);
        uint64_t T[MAXEV];
        int nt = fx_times(f, T);
        uint64_t lo = T[0], hi = T[nt - 1];
        int bits = f->n - 1;
        unsigned long total = 1UL << (bits > 12 ? 12 : bits);
        for (unsigned long cuts = 0; cuts < total; cuts++) {
            c_splits++;
            for (int i = 0; i < nt; i++)
                for (int j = i; j < nt; j++) {
                    uint64_t from = T[i], to = T[j];
                    if (from < lo || to > hi) continue;
                    if (compare_one(f, (unsigned)cuts, from, to, vac) != 0)
                        bad++;
                }
        }
    }
    CHECK(bad == 0, "sweep A: %ld disagreements over %ld (fixture,split,"
          "window) triples", bad, c_triples);
}

/* ── sweep B: window bounds offset by one nanosecond either side ──── */

static void sweep_windows(void)
{
    printf("--- sweep B: window bounds +/- 1 ns (seam inclusivity) ---\n");
    long bad = 0, triples_before = c_triples;
    for (int k = 0; k < g_nfx; k++) {
        const struct fixture *f = &g_fx[k];
        int vac = (strstr(f->name, "no markers") != NULL);
        uint64_t T[MAXEV];
        int nt = fx_times(f, T);
        uint64_t lo = T[0], hi = T[nt - 1];
        uint64_t C[MAXEV * 3];
        int nc = 0;
        for (int i = 0; i < nt; i++) {
            if (T[i] > lo) C[nc++] = T[i] - 1;
            C[nc++] = T[i];
            if (T[i] < hi) C[nc++] = T[i] + 1;
        }
        /* splits: none, every single cut, and all cuts */
        unsigned splits[MAXEV + 2];
        int ns = 0;
        splits[ns++] = 0;
        for (int b = 0; b < f->n - 1 && b < 20; b++) splits[ns++] = 1u << b;
        splits[ns++] = (f->n - 1) >= 20 ? 0xFFFFFu : ((1u << (f->n - 1)) - 1u);
        for (int s = 0; s < ns; s++)
            for (int i = 0; i < nc; i++)
                for (int j = 0; j < nc; j++) {
                    if (C[j] < C[i]) continue;
                    if (C[i] < lo || C[j] > hi) continue;
                    if (compare_one(f, splits[s], C[i], C[j], vac) != 0)
                        bad++;
                }
    }
    CHECK(bad == 0, "sweep B: %ld disagreements over %ld more triples",
          bad, c_triples - triples_before);
}

/* ── literal expectations for the named adversarial shapes ───────── */

static const struct pgwt_execution *find_row(
    const struct pgwt_exec_index_query_result *q, uint32_t pid, uint64_t s)
{
    for (int i = 0; i < q->num_rows; i++)
        if (q->rows[i].pid == pid && q->rows[i].start_ns == s)
            return &q->rows[i];
    return NULL;
}

/* Build one fixture as `blocks` roughly-equal blocks and query the full
 * covered span. Caller frees via pgwt_exec_index_query_free + _free. */
static int full_query(const char *fixture_name, int blocks,
                      struct pgwt_exec_index *idx,
                      struct pgwt_exec_index_query_result *q)
{
    const struct fixture *f = NULL;
    for (int i = 0; i < g_nfx; i++)
        if (strcmp(g_fx[i].name, fixture_name) == 0) f = &g_fx[i];
    if (!f) { fprintf(stderr, "no fixture %s\n", fixture_name); exit(2); }
    unsigned cuts = 0;
    if (blocks > 1) {
        int step = f->n / blocks;
        if (step < 1) step = 1;
        for (int e = step; e < f->n; e += step) cuts |= 1u << (e - 1);
    }
    if (build_index(f, cuts, idx, NULL) != 0)
        return -1;
    return pgwt_exec_index_query(idx, f->ev[0].timestamp_ns,
                                 f->ev[f->n - 1].timestamp_ns, q);
}

static void literal_expectations(void)
{
    printf("--- literal expectations (constants, not re-derivations) ---\n");
    struct pgwt_exec_index idx;
    struct pgwt_exec_index_query_result q;

    /* F1: one row, planned, real end. 4 blocks, so the plan pair, the
     * EXEC_START and the EXEC_END are in different blocks. */
    CHECK(full_query("plain plan+exec+end", 4, &idx, &q) == 0,
          "F1 queries over 4 blocks");
    CHECK(q.num_rows == 1, "F1: exactly 1 row (got %d)", q.num_rows);
    if (q.num_rows == 1) {
        const struct pgwt_execution *r = &q.rows[0];
        CHECK(r->start_ns == BASE + 40 * MS && r->end_ns == BASE + 80 * MS,
              "F1: boundaries are the EXEC markers, +40ms..+80ms");
        CHECK(r->has_plan && r->plan_start_ns == BASE + 20 * MS &&
              r->plan_end_ns == BASE + 30 * MS,
              "F1: the plan pair from the PREVIOUS block is carried over");
        CHECK(!r->in_progress && !r->end_inferred,
              "F1: real EXEC_END, so end_inferred is 0");
        CHECK(r->n_events == PGWT_EXEC_INDEX_NOT_INDEXED &&
              r->n_workers == PGWT_EXEC_INDEX_NOT_INDEXED &&
              r->matches_event_filter == PGWT_EXEC_INDEX_NOT_INDEXED,
              "F1: the three unindexed counts are POISONED (-1), not 0");
        CHECK(q.counts_indexed == 0,
              "F1: counts_indexed == 0 declares the absence");
    }
    pgwt_exec_index_query_free(&q);
    pgwt_exec_index_free(&idx);

    /* F2: open at capture end, with the start in block 1 of 4. */
    CHECK(full_query("open at capture end", 4, &idx, &q) == 0,
          "F2 queries over 4 blocks");
    CHECK(q.num_rows == 1 && q.rows[0].in_progress &&
          q.rows[0].end_ns == 0,
          "F2: the execution open at capture end survives as in_progress "
          "with end_ns 0 (rows=%d)", q.num_rows);
    CHECK(pgwt_exec_index_open_at_end(&idx) == 1,
          "F2: open_at_end reports the 1 carried-over execution");
    pgwt_exec_index_query_free(&q);
    pgwt_exec_index_free(&idx);
    /* ...and a window starting AFTER its EXEC_START still attributes it. */
    {
        struct pgwt_exec_index i2;
        struct pgwt_exec_index_query_result q2;
        CHECK(full_query("open at capture end", 4, &i2, &q2) == 0, "F2 rebuild");
        pgwt_exec_index_query_free(&q2);
        int rc = pgwt_exec_index_query(&i2, BASE + 40 * MS, BASE + 70 * MS,
                                       &q2);
        CHECK(rc == 0 && q2.num_rows == 1 && q2.rows[0].started_before_window
              && q2.rows[0].in_progress,
              "F2: a window that OPENS mid-execution still returns it, "
              "flagged started_before_window (rc=%d rows=%d)", rc,
              q2.num_rows);
        pgwt_exec_index_query_free(&q2);
        pgwt_exec_index_free(&i2);
    }

    /* F3: three or more blocks spanned. */
    CHECK(full_query("one execution across many blocks", 7, &idx, &q) == 0,
          "F3 queries over 7 blocks");
    CHECK(q.num_rows == 1 && q.rows[0].start_ns == BASE + 10 * MS &&
          q.rows[0].end_ns == BASE + 70 * MS && !q.rows[0].in_progress,
          "F3: one execution reassembled across 7 blocks (rows=%d)",
          q.num_rows);
    CHECK(idx.n_chunks == 7 && span_blocks(&idx, 0) == 7,
          "F3: that execution genuinely spans %d of %d blocks",
          idx.n_rows ? span_blocks(&idx, 0) : -1, idx.n_chunks);
    pgwt_exec_index_query_free(&q);
    pgwt_exec_index_free(&idx);

    /* F4: the #222 shape. */
    CHECK(full_query("#222: two starts, one end, CMD_END closes the orphan",
                     4, &idx, &q) == 0, "F4 queries over 4 blocks");
    CHECK(q.num_rows == 2, "F4: both executions are present (got %d)",
          q.num_rows);
    {
        const struct pgwt_execution *outer = find_row(&q, P1, BASE + 10 * MS);
        const struct pgwt_execution *inner = find_row(&q, P1, BASE + 20 * MS);
        CHECK(inner && !inner->in_progress && inner->end_ns == BASE + 30 * MS
              && !inner->end_inferred,
              "F4: the EXEC_END closes the INNER row at +30ms");
        CHECK(outer && !outer->in_progress && outer->end_ns == BASE + 40 * MS
              && outer->end_inferred,
              "F4: CMD_END closes the orphaned OUTER row at +40ms, "
              "end_inferred=1");
    }
    pgwt_exec_index_query_free(&q);
    pgwt_exec_index_free(&idx);

    /* F11: three orphans, one CMD_END. */
    CHECK(full_query("three orphans closed by one CMD_END", 3, &idx, &q) == 0,
          "F11 queries over 3 blocks");
    CHECK(q.num_rows == 3, "F11: all three rows present (got %d)",
          q.num_rows);
    {
        int inferred = 0;
        for (int i = 0; i < q.num_rows; i++)
            if (!q.rows[i].in_progress && q.rows[i].end_ns == BASE + 40 * MS
                && q.rows[i].end_inferred) inferred++;
        CHECK(inferred == 3,
              "F11: one CMD_END closed all 3 at +40ms, each end_inferred "
              "(got %d)", inferred);
    }
    pgwt_exec_index_query_free(&q);
    pgwt_exec_index_free(&idx);

    /* F10: query_id only on EXEC_END. */
    {
        struct pgwt_exec_index i2;
        struct pgwt_exec_index_query_result q2;
        CHECK(full_query("query_id filled by EXEC_END only", 2, &i2, &q2) == 0,
              "F10 queries over 2 blocks");
        CHECK(q2.num_rows == 1 && q2.rows[0].query_id == 91,
              "F10: query_id 91 is taken from the EXEC_END when the start "
              "carried 0 (got %llu)",
              q2.num_rows ? (unsigned long long)q2.rows[0].query_id : 0ULL);
        pgwt_exec_index_query_free(&q2);
        int rc = pgwt_exec_index_query(&i2, BASE + 10 * MS, BASE + 20 * MS,
                                       &q2);
        CHECK(rc == 0 && q2.num_rows == 1 && q2.rows[0].query_id == 0 &&
              q2.rows[0].in_progress,
              "F10: a window ENDING before that EXEC_END reports query_id 0 "
              "and in_progress, exactly as raw does (rc=%d)", rc);
        pgwt_exec_index_query_free(&q2);
        pgwt_exec_index_free(&i2);
    }

    /* F9 / F8: has_plan must be 0. */
    CHECK(full_query("plan query_id mismatch", 2, &idx, &q) == 0, "F9 query");
    CHECK(q.num_rows == 1 && !q.rows[0].has_plan,
          "F9: a plan whose query_id differs is NOT attached");
    pgwt_exec_index_query_free(&q);
    pgwt_exec_index_free(&idx);
    CHECK(full_query("CMD_END discards a ready plan", 2, &idx, &q) == 0,
          "F8 query");
    CHECK(q.num_rows == 1 && !q.rows[0].has_plan,
          "F8: CMD_END discarded the ready plan, so has_plan is 0");
    pgwt_exec_index_query_free(&q);
    pgwt_exec_index_free(&idx);

    /* F12: ABSENT, not wrong — and not a refusal. */
    CHECK(full_query("no markers at all (absent, not wrong)", 2, &idx, &q)
          == 0, "F12 query");
    CHECK(q.num_rows == 0 && q.refused == PGWT_EXEC_INDEX_OK,
          "F12: a window with genuinely no executions returns 0 rows with "
          "refused=OK — the ONE encoding of 'none', distinct from every "
          "refusal");
    pgwt_exec_index_query_free(&q);
    pgwt_exec_index_free(&idx);
}

/* ── MARKERS ONLY: dropping every non-marker changes nothing ────── */

static void markers_only_equivalence(void)
{
    printf("--- MARKERS ONLY: non-marker records cannot move a boundary ---\n");
    long checked = 0, bad = 0, with_waits = 0;
    for (int k = 0; k < g_nfx; k++) {
        struct fixture *f = &g_fx[k];
        struct fixture m;
        memset(&m, 0, sizeof(m));
        m.name = f->name;
        for (int i = 0; i < f->n; i++)
            if (PGWT_IS_MARKER(f->ev[i].old_event))
                m.ev[m.n++] = f->ev[i];
        if (m.n == 0) continue;
        if (m.n < f->n) with_waits++;

        struct pgwt_exec_index a, b;
        if (build_index(f, 0, &a, NULL) != 0 ||
            build_index(&m, 0, &b, NULL) != 0) { bad++; continue; }
        if (a.n_rows != b.n_rows) {
            printf("  FAIL: %s: %d rows with waits, %d without\n",
                   f->name, a.n_rows, b.n_rows);
            bad++;
        } else {
            for (int i = 0; i < a.n_rows; i++) {
                const struct pgwt_exec_index_entry *x = &a.rows[i];
                const struct pgwt_exec_index_entry *y = &b.rows[i];
                if (memcmp(x, y, sizeof(*x)) != 0) {
                    printf("  FAIL: %s row %d differs when waits are "
                           "dropped\n", f->name, i);
                    bad++;
                }
            }
        }
        checked += a.n_rows;
        pgwt_exec_index_free(&a);
        pgwt_exec_index_free(&b);
    }
    CHECK(bad == 0, "markers-only build is byte-identical over %ld rows",
          checked);
    CHECK(with_waits >= 6,
          "%ld fixtures actually HAD non-marker records to drop (otherwise "
          "this check is vacuous)", with_waits);
}

/* ── mutation probes: the comparator must see one of everything ──── */

static void mutation_probes(void)
{
    printf("--- mutation probes: the comparator can see one of each ---\n");
    struct pgwt_execution a, b;
    memset(&a, 0, sizeof(a));
    a.pid = P1; a.query_id = 7; a.start_ns = BASE + 40 * MS;
    a.end_ns = BASE + 80 * MS; a.plan_start_ns = BASE + 20 * MS;
    a.plan_end_ns = BASE + 30 * MS; a.has_plan = 1; a.in_progress = 0;
    a.end_inferred = 0; a.started_before_window = 0;

    CHECK(exec_boundary_diff(&a, &a) == -1,
          "identical rows compare equal (the probe's own baseline)");
    for (int fld = 0; fld < N_BOUNDARY_FIELDS; fld++) {
        b = a;
        switch (fld) {
        case 0: b.pid += 1; break;
        case 1: b.query_id += 1; break;
        case 2: b.start_ns += 1; break;            /* one nanosecond */
        case 3: b.end_ns += 1; break;              /* one nanosecond */
        case 4: b.plan_start_ns += 1; break;
        case 5: b.plan_end_ns += 1; break;
        case 6: b.has_plan = 0; break;
        case 7: b.in_progress = 1; break;
        case 8: b.end_inferred = 1; break;
        case 9: b.started_before_window = 1; break;
        }
        int d = exec_boundary_diff(&a, &b);
        CHECK(d == fld, "a one-unit change in %s is detected (reported %s)",
              g_field_names[fld],
              d >= 0 ? g_field_names[d] : "NOTHING");
    }

    /* And the differential can go red end to end: corrupt one index row and
     * the (fixture, split, window) comparison must fail. A gate nobody has
     * seen fail is not evidence that it can. */
    struct pgwt_exec_index idx;
    struct pgwt_exec_index_query_result q;
    const struct fixture *f = NULL;
    for (int i = 0; i < g_nfx; i++)
        if (strcmp(g_fx[i].name, "plain plan+exec+end") == 0) f = &g_fx[i];
    if (build_index(f, 0, &idx, NULL) == 0 && idx.n_rows == 1) {
        idx.rows[0].close_ns += 1;        /* one nanosecond, deliberately */
        int rc = pgwt_exec_index_query(&idx, f->ev[0].timestamp_ns,
                                       f->ev[f->n - 1].timestamp_ns, &q);
        struct pgwt_executions_result ref;
        oracle_rows(f, f->ev[0].timestamp_ns, f->ev[f->n - 1].timestamp_ns,
                    &ref);
        int saw = (rc == 0 && q.num_rows == 1 && ref.num_rows == 1)
                ? exec_boundary_diff(&q.rows[0], &ref.rows[0]) : -2;
        CHECK(saw == 3,
              "END TO END RED: a 1 ns corruption of one indexed close_ns is "
              "caught by the differential as `end_ns` (reported %d)", saw);
        free(ref.rows);
        pgwt_exec_index_query_free(&q);
    } else {
        CHECK(0, "mutation setup failed");
    }
    pgwt_exec_index_free(&idx);
}

/* ── bypass suite: every way the index could approve blindly ────── */

static struct fixture *fx_by_name(const char *name)
{
    for (int i = 0; i < g_nfx; i++)
        if (strcmp(g_fx[i].name, name) == 0) return &g_fx[i];
    exit(2);
}

static void expect_refusal(const char *what, struct pgwt_exec_index *idx,
                           uint64_t from, uint64_t to,
                           enum pgwt_exec_index_refusal want)
{
    struct pgwt_exec_index_query_result q;
    int rc = pgwt_exec_index_query(idx, from, to, &q);
    CHECK(rc == -1 && q.refused == want && q.rows == NULL &&
          q.num_rows == 0,
          "%s REFUSES with %s and no rows (rc=%d got=%s rows=%d)", what,
          pgwt_exec_index_refusal_str(want), rc,
          pgwt_exec_index_refusal_str(q.refused), q.num_rows);
    pgwt_exec_index_query_free(&q);
}

static void bypass_suite(void)
{
    printf("--- bypass suite: a gate that cannot see must REFUSE ---\n");
    struct fixture *f = fx_by_name("plain plan+exec+end");
    uint64_t lo = f->ev[0].timestamp_ns, hi = f->ev[f->n - 1].timestamp_ns;

    /* 1. NULL index. */
    {
        struct pgwt_exec_index_query_result q;
        int rc = pgwt_exec_index_query(NULL, lo, hi, &q);
        CHECK(rc == -1 && q.refused == PGWT_EXEC_INDEX_REFUSE_NULL,
              "a NULL index REFUSES (rc=%d)", rc);
    }

    /* 2. Nothing indexed at all: sealed, but EMPTY, not "no executions". */
    {
        struct pgwt_exec_index idx;
        pgwt_exec_index_init(&idx);
        CHECK(pgwt_exec_index_seal(&idx) == 0, "an empty index may be sealed");
        expect_refusal("an index with no blocks", &idx, lo, hi,
                       PGWT_EXEC_INDEX_REFUSE_EMPTY);
        pgwt_exec_index_free(&idx);
    }

    /* 3. Unsealed. */
    {
        struct pgwt_exec_index idx;
        pgwt_exec_index_init(&idx);
        CHECK(pgwt_exec_index_add_block(&idx, 0, lo, hi, f->ev, f->n) == 0,
              "a block is accepted");
        expect_refusal("an UNSEALED index", &idx, lo, hi,
                       PGWT_EXEC_INDEX_REFUSE_UNSEALED);
        pgwt_exec_index_free(&idx);
    }

    /* 4. Wrong version — the #315 lesson: a version is a contract, and an
     *    index stamped with another one must not be read. */
    {
        struct pgwt_exec_index idx;
        CHECK(build_index(f, 0, &idx, NULL) == 0, "a v1 index builds");
        idx.version = PGWT_EXEC_INDEX_VERSION + 1;
        expect_refusal("an index of an UNKNOWN version", &idx, lo, hi,
                       PGWT_EXEC_INDEX_REFUSE_VERSION);
        idx.version = PGWT_EXEC_INDEX_VERSION;
        pgwt_exec_index_free(&idx);
    }

    /* 5. A PARTIAL block range: block 1 skipped. The events of a missing
     *    block are simply not there, and a missing EXEC_END is
     *    indistinguishable from a still-open execution — so this must
     *    refuse, not answer. */
    {
        struct pgwt_exec_index idx;
        pgwt_exec_index_init(&idx);
        CHECK(pgwt_exec_index_add_block(&idx, 0, f->ev[0].timestamp_ns,
                                        f->ev[1].timestamp_ns, &f->ev[0], 2)
              == 0, "block 0 accepted");
        CHECK(pgwt_exec_index_add_block(&idx, 2, f->ev[4].timestamp_ns,
                                        f->ev[5].timestamp_ns, &f->ev[4], 2)
              == -1, "block 2 after block 0 is REJECTED at add time");
        CHECK(pgwt_exec_index_seal(&idx) == -1,
              "sealing a gapped feed fails");
        expect_refusal("a feed that SKIPPED a block", &idx, lo, hi,
                       PGWT_EXEC_INDEX_REFUSE_GAP);
        pgwt_exec_index_free(&idx);
    }

    /* 6. A reordered feed: block bounds going backwards. */
    {
        struct pgwt_exec_index idx;
        pgwt_exec_index_init(&idx);
        (void)pgwt_exec_index_add_block(&idx, 0, f->ev[4].timestamp_ns,
                                        f->ev[5].timestamp_ns, &f->ev[4], 2);
        CHECK(pgwt_exec_index_add_block(&idx, 1, f->ev[0].timestamp_ns,
                                        f->ev[1].timestamp_ns, &f->ev[0], 2)
              == -1, "a block whose bounds go BACKWARDS is rejected");
        expect_refusal("a REORDERED block feed", &idx, lo, hi,
                       PGWT_EXEC_INDEX_REFUSE_GAP);
        pgwt_exec_index_free(&idx);
    }

    /* 7. Records out of order inside one block. */
    {
        struct pgwt_exec_index idx;
        struct pgwt_trace_event rev[MAXEV];
        for (int i = 0; i < f->n; i++) rev[i] = f->ev[f->n - 1 - i];
        pgwt_exec_index_init(&idx);
        CHECK(pgwt_exec_index_add_block(&idx, 0, lo, hi, rev, f->n) == -1,
              "records in DESCENDING timestamp order are rejected");
        expect_refusal("an out-of-order record stream", &idx, lo, hi,
                       PGWT_EXEC_INDEX_REFUSE_GAP);
        pgwt_exec_index_free(&idx);
    }

    /* 8. A record outside its own block's declared bounds — a file/caller
     *    inconsistency, never silently clamped. */
    {
        struct pgwt_exec_index idx;
        pgwt_exec_index_init(&idx);
        CHECK(pgwt_exec_index_add_block(&idx, 0, lo, hi - 1, f->ev, f->n)
              == -1, "a record past the block's last_timestamp is rejected");
        pgwt_exec_index_free(&idx);
        pgwt_exec_index_init(&idx);
        CHECK(pgwt_exec_index_add_block(&idx, 0, lo + 1, hi, f->ev, f->n)
              == -1, "a record before the block's first_timestamp is "
              "rejected");
        pgwt_exec_index_free(&idx);
    }

    /* 9. A window the index does not fully cover, both directions. */
    {
        struct pgwt_exec_index idx;
        CHECK(build_index(f, 0, &idx, NULL) == 0, "index builds");
        expect_refusal("a window starting BEFORE coverage", &idx, lo - 1, hi,
                       PGWT_EXEC_INDEX_REFUSE_RANGE);
        expect_refusal("a window ending AFTER coverage", &idx, lo, hi + 1,
                       PGWT_EXEC_INDEX_REFUSE_RANGE);
        expect_refusal("an inverted window", &idx, hi, lo,
                       PGWT_EXEC_INDEX_REFUSE_BAD_WINDOW);
        pgwt_exec_index_free(&idx);
    }

    /* 10. A block with bounds but no events: coverage must still extend
     *     (so the following window is answerable) and the row set must be
     *     unchanged. */
    {
        struct pgwt_exec_index idx;
        pgwt_exec_index_init(&idx);
        CHECK(pgwt_exec_index_add_block(&idx, 0, lo, hi, f->ev, f->n) == 0,
              "block 0 with the whole fixture");
        CHECK(pgwt_exec_index_add_block(&idx, 1, hi + 1, hi + 100, NULL, 0)
              == 0, "an EMPTY block (bounds, no records) is accepted");
        CHECK(pgwt_exec_index_seal(&idx) == 0, "it seals");
        struct pgwt_exec_index_query_result q;
        int rc = pgwt_exec_index_query(&idx, lo, hi + 100, &q);
        CHECK(rc == 0 && q.num_rows == 1,
              "the empty block EXTENDED coverage without adding rows "
              "(rc=%d rows=%d)", rc, q.num_rows);
        CHECK(idx.cover_to_ns == hi + 100,
              "coverage comes from the block HEADER, not from its records");
        pgwt_exec_index_query_free(&q);
        pgwt_exec_index_free(&idx);
    }

    /* 11. Injected allocation failures. Every one must come back as a
     *     REFUSAL, never as a short-but-plausible row set. */
    {
        const char *points[] = { "exec_index_rows", "exec_index_chunks",
                                 "exec_index_pids", "exec_index_open_rows",
                                 "pid_index_grow" };
        struct fixture *big = fx_by_name("three orphans closed by one "
                                         "CMD_END");
        uint64_t blo = big->ev[0].timestamp_ns;
        uint64_t bhi = big->ev[big->n - 1].timestamp_ns;
        for (unsigned p = 0; p < sizeof(points) / sizeof(points[0]); p++) {
            setenv("PGWT_TEST_ALLOC_FAIL", points[p], 1);
            struct pgwt_exec_index idx;
            int built = build_index(big, 0x5, &idx, NULL);
            struct pgwt_exec_index_query_result q;
            int rc = pgwt_exec_index_query(&idx, blo, bhi, &q);
            CHECK(built == -1 && rc == -1 && q.num_rows == 0 &&
                  (q.refused == PGWT_EXEC_INDEX_REFUSE_ALLOC ||
                   q.refused == PGWT_EXEC_INDEX_REFUSE_BUILD_FAILED),
                  "an allocation failure at %s REFUSES (built=%d rc=%d "
                  "refused=%s rows=%d)", points[p], built, rc,
                  pgwt_exec_index_refusal_str(q.refused), q.num_rows);
            pgwt_exec_index_query_free(&q);
            pgwt_exec_index_free(&idx);
            unsetenv("PGWT_TEST_ALLOC_FAIL");
        }
        /* The injection hook itself must be inert for an unknown point —
         * otherwise the five checks above could be passing for the wrong
         * reason (everything failing, always). */
        setenv("PGWT_TEST_ALLOC_FAIL", "no_such_injection_point", 1);
        struct pgwt_exec_index idx;
        struct pgwt_exec_index_query_result q;
        int built = build_index(big, 0x5, &idx, NULL);
        int rc = pgwt_exec_index_query(&idx, blo, bhi, &q);
        CHECK(built == 0 && rc == 0 && q.num_rows == 3,
              "an UNKNOWN injection point changes nothing (built=%d rc=%d "
              "rows=%d) — so the five above failed for the right reason",
              built, rc, q.num_rows);
        pgwt_exec_index_query_free(&q);
        pgwt_exec_index_free(&idx);
        unsetenv("PGWT_TEST_ALLOC_FAIL");
    }

    /* 12. The contract's clause (b) is LOAD-BEARING, not decoration. The
     *     index agrees with pgwt_compute_executions over an array truncated
     *     at to_ns — which is what src/server.c builds. Over an UNtruncated
     *     array the reference answers differently (a late EXEC_START pushes
     *     no row, so a later EXEC_END pops and closes the EARLIER one), and
     *     this shows that difference rather than asserting it away. */
    {
        struct fixture lf;
        memset(&lf, 0, sizeof(lf));
        lf.name = "late-start";
        fx_add(&lf, mk(BASE + 10 * MS, P1, PGWT_MARKER_EXEC_START, 201));
        fx_add(&lf, mk(BASE + 50 * MS, P1, PGWT_MARKER_EXEC_START, 202));
        fx_add(&lf, mk(BASE + 60 * MS, P1, PGWT_MARKER_EXEC_END, 202));
        uint64_t from = BASE + 10 * MS, to = BASE + 30 * MS;

        struct pgwt_exec_index idx;
        struct pgwt_exec_index_query_result q;
        CHECK(build_index(&lf, 0, &idx, NULL) == 0, "late-start index builds");
        /* coverage is [10ms, 60ms]; the window is inside it */
        int rc = pgwt_exec_index_query(&idx, from, to, &q);

        struct pgwt_executions_result trunc, untrunc;
        oracle_rows(&lf, from, to, &trunc);
        pgwt_compute_executions(lf.ev, lf.n, from, to, NULL, NULL, 0,
                                &untrunc);

        CHECK(rc == 0 && q.num_rows == 1 && trunc.num_rows == 1 &&
              exec_boundary_diff(&q.rows[0], &trunc.rows[0]) == -1,
              "clause (b) honoured: index == reference over the array the "
              "server actually builds (rc=%d)", rc);
        CHECK(untrunc.num_rows == 1 && !untrunc.rows[0].in_progress &&
              q.num_rows == 1 && q.rows[0].in_progress,
              "clause (b) is LOAD-BEARING: fed records past to_ns the "
              "reference closes that row (in_progress=%d) where the index "
              "reports it open (%d) — so the clause is a real precondition",
              untrunc.num_rows ? untrunc.rows[0].in_progress : -1,
              q.num_rows ? q.rows[0].in_progress : -1);
        free(trunc.rows);
        free(untrunc.rows);
        pgwt_exec_index_query_free(&q);
        pgwt_exec_index_free(&idx);
    }
}

/* ── the falsifiable prediction, measured ─────────────────── */

static void measure_complexity(void)
{
    printf("--- prediction: O(events in window) -> O(executions in window) "
           "---\n");
    const int N_EXEC = 2000;
    const int WAITS_PER_EXEC = 200;
    const int BLOCK_EVENTS = 4096;
    const uint64_t STEP = 100000ULL;     /* 100 us between records */

    int total = N_EXEC * (WAITS_PER_EXEC + 2);
    struct pgwt_trace_event *ev = (struct pgwt_trace_event *)
        malloc((size_t)total * sizeof(*ev));
    if (!ev) { CHECK(0, "fixture allocation"); return; }
    int n = 0;
    uint64_t t = BASE;
    for (int e = 0; e < N_EXEC; e++) {
        uint32_t pid = 5000 + (uint32_t)(e % 64);
        ev[n++] = mk(t, pid, PGWT_MARKER_EXEC_START, 900 + (uint64_t)(e % 7));
        t += STEP;
        for (int w = 0; w < WAITS_PER_EXEC; w++) {
            ev[n++] = wt(t, pid, STEP / 2, 900 + (uint64_t)(e % 7));
            t += STEP;
        }
        ev[n++] = mk(t, pid, PGWT_MARKER_EXEC_END, 900 + (uint64_t)(e % 7));
        t += STEP;
    }

    struct pgwt_exec_index idx;
    pgwt_exec_index_init(&idx);
    int blk = 0;
    for (int s = 0; s < n; s += BLOCK_EVENTS) {
        int cnt = (s + BLOCK_EVENTS <= n) ? BLOCK_EVENTS : (n - s);
        if (pgwt_exec_index_add_block(&idx, blk++, ev[s].timestamp_ns,
                                      ev[s + cnt - 1].timestamp_ns,
                                      &ev[s], cnt) != 0) {
            CHECK(0, "large-fixture index build");
            free(ev);
            pgwt_exec_index_free(&idx);
            return;
        }
    }
    CHECK(pgwt_exec_index_seal(&idx) == 0, "large index seals (%d blocks, "
          "%d rows, %d events)", idx.n_chunks, idx.n_rows, n);

    /* A window over the middle 10%. */
    uint64_t span = ev[n - 1].timestamp_ns - ev[0].timestamp_ns;
    uint64_t from = ev[0].timestamp_ns + span * 45 / 100;
    uint64_t to = ev[0].timestamp_ns + span * 55 / 100;

    int events_in_window = 0;
    for (int i = 0; i < n; i++)
        if (ev[i].timestamp_ns >= from && ev[i].timestamp_ns <= to)
            events_in_window++;

    struct pgwt_exec_index_query_result q;
    struct timespec a0, a1;
    clock_gettime(CLOCK_MONOTONIC, &a0);
    int rc = pgwt_exec_index_query(&idx, from, to, &q);
    clock_gettime(CLOCK_MONOTONIC, &a1);
    double idx_us = (a1.tv_sec - a0.tv_sec) * 1e6
                  + (a1.tv_nsec - a0.tv_nsec) / 1e3;
    CHECK(rc == 0, "large query succeeds");

    /* The raw path, in the server's own input shape. */
    struct pgwt_trace_event *arr = (struct pgwt_trace_event *)
        malloc((size_t)n * sizeof(*arr));
    int m = 0;
    for (int i = 0; i < n; i++)
        if (ev[i].timestamp_ns < from && PGWT_IS_MARKER(ev[i].old_event))
            arr[m++] = ev[i];
    for (int i = 0; i < n; i++)
        if (ev[i].timestamp_ns >= from && ev[i].timestamp_ns <= to)
            arr[m++] = ev[i];
    struct pgwt_executions_result ref;
    struct timespec b0, b1;
    clock_gettime(CLOCK_MONOTONIC, &b0);
    pgwt_compute_executions(arr, m, from, to, NULL, NULL, 0, &ref);
    clock_gettime(CLOCK_MONOTONIC, &b1);
    double raw_us = (b1.tv_sec - b0.tv_sec) * 1e6
                  + (b1.tv_nsec - b0.tv_nsec) / 1e3;

    CHECK(!ref.failed && q.num_rows == ref.num_rows,
          "large fixture: index %d rows == raw %d rows", q.num_rows,
          ref.num_rows);
    int bad = 0;
    for (int i = 0; i < q.num_rows && i < ref.num_rows; i++)
        if (exec_boundary_diff(&q.rows[i], &ref.rows[i]) >= 0) bad++;
    CHECK(bad == 0, "large fixture: every row agrees bit-exactly");

    /* The gate on the prediction. Deterministic counts, not timings. */
    CHECK(q.rows_examined <= 4L * q.num_rows + 64,
          "work is O(executions in window): rows_examined=%ld for "
          "num_rows=%d", q.rows_examined, q.num_rows);
    CHECK(q.rows_examined * 10 < events_in_window,
          "rows_examined=%ld is more than 10x below events_in_window=%d",
          q.rows_examined, events_in_window);
    CHECK(q.chunks_total - q.chunks_scanned > 0,
          "the chunk prefilter actually SKIPPED %ld of %ld chunks (it is "
          "not a no-op)", q.chunks_total - q.chunks_scanned, q.chunks_total);

    printf("  MEASURED n=1: events_total=%d events_in_window=%d "
           "index_rows_total=%d rows_examined=%ld rows_returned=%d "
           "chunks_total=%ld chunks_scanned=%ld "
           "index_query_us=%.1f raw_recompute_us=%.1f (timings REPORTED, "
           "not asserted)\n",
           n, events_in_window, idx.n_rows, q.rows_examined, q.num_rows,
           q.chunks_total, q.chunks_scanned, idx_us, raw_us);

    free(ref.rows);
    free(arr);
    pgwt_exec_index_query_free(&q);
    pgwt_exec_index_free(&idx);
    free(ev);
}

/* ── non-vacuity: the sweeps must have exercised what they claim ── */

static void non_vacuity(void)
{
    printf("--- non-vacuity: the sweeps really did see these shapes ---\n");
    CHECK(c_splits > 1, "more than one block split was used (%ld) — a "
          "one-block-only suite never tests carry-over", c_splits);
    CHECK(c_triples > 1000, "%ld (fixture,split,window) triples compared",
          c_triples);
    CHECK(c_rows > 0, "%ld rows were actually compared (not 0 == 0)", c_rows);
    CHECK(c_crossing > 0, "%ld executions spanned a BLOCK BOUNDARY",
          c_crossing);
    CHECK(c_span3 > 0, "%ld executions spanned THREE OR MORE blocks",
          c_span3);
    CHECK(c_inprog > 0, "%ld returned rows were in_progress", c_inprog);
    CHECK(c_before > 0, "%ld returned rows were started_before_window",
          c_before);
    CHECK(c_inferred > 0, "%ld returned rows were CMD_END-inferred ends",
          c_inferred);
    CHECK(c_plan > 0, "%ld returned rows carried a plan pair", c_plan);
    CHECK(c_zero_len > 0, "%ld returned rows were zero-length (start==end)",
          c_zero_len);
    CHECK(c_multipid > 0, "%ld windows returned rows for more than one pid",
          c_multipid);
    CHECK(c_skipped > 0, "the prefilter skipped a chunk %ld times across the "
          "sweeps", c_skipped);
    CHECK(c_mismatch == 0, "no disagreement anywhere%s%s",
          c_mismatch ? " — first: " : "",
          c_mismatch ? g_first_mismatch : "");
}

int main(void)
{
    printf("=== test_exec_index: the executions index (Phase 3) ===\n");
    printf("index version under test: %d\n", PGWT_EXEC_INDEX_VERSION);
    build_fixtures();
    printf("%d fixtures\n", g_nfx);

    literal_expectations();
    markers_only_equivalence();
    mutation_probes();
    sweep_splits();
    sweep_windows();
    bypass_suite();
    measure_complexity();
    non_vacuity();

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    printf("%s\n", tests_failed ? "FAILED" : "PASSED");
    return tests_failed ? 1 : 0;
}
