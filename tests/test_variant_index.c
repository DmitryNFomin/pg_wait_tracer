/* test_variant_index.c — the per-execution step-sequence index (3b).
 *
 * WHAT IS BEING PINNED
 * --------------------
 * src/variant_index.c claims that one window query over an append-only
 * per-execution sequence index returns BIT-EXACTLY the
 * struct pgwt_variants_result that pgwt_compute_variants() returns for the
 * same window, filter, max_variants and phase, while touching
 * O(executions in window) index rows instead of O(events in window). Both
 * halves are checked: the agreement, field by field with no epsilon
 * anywhere (including the raw bits of the avg_loop_n double), and the work
 * actually done, counted.
 *
 * THE ORACLE IS THE SHIPPING FUNCTION. pgwt_compute_variants (src/compute.c)
 * is linked in and called. variant_index.c is an independent
 * reimplementation of the marker walk, the loop compression and the hash,
 * so a mistake on either side shows up as a disagreement. Nothing is shared
 * between them but the result struct.
 *
 * THE ORACLE'S INPUT IS THE SERVER'S INPUT. src/server.c handle_variants()
 * hands pgwt_compute_variants every record in [from, to] — selection by the
 * record's END timestamp, INCLUSIVE AT BOTH ENDS (src/server.c:2848-2850) —
 * and nothing else: no markers-only prefix, unlike load_execution_rows().
 * oracle_variants() below builds exactly that array and passes the same
 * all-zero filter handle_variants passes, so agreement here is agreement
 * with what the product computes.
 *
 * WHAT MAKES THIS PASS WHILE BEING WRONG — and what stops it
 * ----------------------------------------------------------
 * 1. A sequence TRUNCATED at a block boundary. Two different executions cut
 *    at the same step hash to the same "variant", and both sides would have
 *    to be wrong the same way to hide it — but a suite that feeds every
 *    fixture as ONE block never cuts anything, so the carry-over is never
 *    exercised and every bug in it is invisible. Stopped by sweeping cut
 *    sets of every fixture, plus the ledger counters c_crossing (an
 *    execution whose markers sit in different blocks) and c_steps_crossing
 *    (an execution whose WAIT RECORDS are spread over two or more blocks —
 *    the one that actually exercises partial-sequence carry-over, as
 *    opposed to merely a straddling marker pair). Falsified on purpose:
 *    PGWT_VARIANT_INDEX_ONE_BLOCK=1 reruns the sweeps with no cuts at all
 *    and REQUIRES those counters to be zero, so the ledger is demonstrated
 *    to be a real gate rather than assumed to be one. That mode can never
 *    exit 0.
 * 2. A sequence DUPLICATED across a merged block and an edge decode. A row
 *    is appended once, at its closing marker, in the block where that
 *    marker lives; any double-append shows up as exec_count and total_ns
 *    disagreeing with the oracle on every window containing it.
 * 3. Vacuity. "index == oracle" is satisfied by 0 == 0, which is how a
 *    whole suite passes against an index that returns nothing. Stopped by
 *    named ledger counters: variants compared, executions selected,
 *    multi-step patterns, loop steps, CPU-only patterns, discarded #222
 *    pairs, idle records dropped, impossible-duration records dropped,
 *    prefilter skips, and order-sensitive pairs are each asserted > 0.
 * 4. A comparator that cannot see. Stopped by the mutation probes: every
 *    compared field is perturbed by the smallest possible amount on a real
 *    result and the comparator must name that exact field.
 * 5. A gate that cannot establish an answer but approves anyway. Stopped by
 *    the bypass suite: an empty index, an unsealed index, a wrong version,
 *    a skipped or reordered block, an out-of-order record, a record outside
 *    its block's bounds, a window outside coverage, an inverted window, an
 *    unknown phase, a NULL filter, a non-empty filter, a NULL index and
 *    every injected allocation failure must each REFUSE with their own
 *    reason, produce no variants, and poison the counts. "No index" must
 *    never read as "no executions".
 *
 * WHAT DEPENDS ON TIMING OR ORDERING. Nothing wall-clock is asserted: the
 * one timing number printed is REPORTED, and the complexity claim is
 * asserted on COUNTED work instead. Event ORDER is load-bearing (collision
 * chains, query_id-set order, the bounded p95 sample, and the IEEE
 * non-associativity of the avg_loop_n double sum) and is pinned by the
 * index refusing a decreasing-timestamp feed rather than reordering it, and
 * by both sides accumulating in closing-marker order. Equal timestamps keep
 * feed order on both sides.
 *
 * Pure: links ../src/variant_index.c + ../src/compute.c. No daemon, no
 * PostgreSQL, no root, no files, no network.
 */
#include "variant_index.h"
#include "compute.h"
#include "idle_rule.h"
#include "pg_wait_tracer.h"
#include "summary_reader.h"
#include "wait_event.h"

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

#define WE_A  ((PG_WAIT_IO << 24) | 21U)
#define WE_B  ((PG_WAIT_IO << 24) | 22U)
#define WE_C  ((PG_WAIT_LWLOCK << 24) | 5U)
#define WE_D  ((PG_WAIT_IO << 24) | 7U)
#define WE_IDLE PG_WAIT_CLIENT_READ          /* pgwt_is_idle_event() == 1 */

/* ── fixtures ─────────────────────────────────────────────── */

#define MAXEV 180

struct fixture {
    const char *name;
    struct pgwt_trace_event ev[MAXEV];
    int n;
    int heavy;          /* reduced cut-set / window sweep (big fixture) */
    int vacuous_ok;     /* legitimately yields no executions */
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

static struct pgwt_trace_event wt(uint64_t ts, uint32_t pid, uint32_t wei,
                                  uint64_t dur, uint64_t qid)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = ts;
    e.pid = pid;
    e.old_event = wei;
    e.new_event = 0;
    e.duration_ns = dur;
    e.query_id = qid;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return e;
}

static struct fixture g_fx[32];
static int g_nfx = 0;

static struct fixture *new_fx(const char *name)
{
    struct fixture *f = &g_fx[g_nfx++];
    memset(f, 0, sizeof(*f));
    f->name = name;
    return f;
}

static void fx_add(struct fixture *f, struct pgwt_trace_event e)
{
    if (f->n >= MAXEV) { printf("  FAIL: fixture %s overflow\n", f->name); return; }
    f->ev[f->n++] = e;
}

#define ES PGWT_MARKER_EXEC_START
#define EE PGWT_MARKER_EXEC_END
#define PS PGWT_MARKER_PLAN_START
#define PE PGWT_MARKER_PLAN_END
#define CE PGWT_MARKER_CMD_END

static void build_fixtures(void)
{
    struct fixture *f;
    uint64_t t;

    f = new_fx("one exec one wait");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 7));
    fx_add(f, wt(BASE + 10 * MS, 101, WE_A, 5 * MS, 0));
    fx_add(f, mk(BASE + 20 * MS, 101, EE, 7));

    f = new_fx("two execs same pattern");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 7));
    fx_add(f, wt(BASE + 10 * MS, 101, WE_A, 5 * MS, 0));
    fx_add(f, mk(BASE + 20 * MS, 101, EE, 7));
    fx_add(f, mk(BASE + 30 * MS, 101, ES, 7));
    fx_add(f, wt(BASE + 40 * MS, 101, WE_A, 9 * MS, 0));
    fx_add(f, mk(BASE + 50 * MS, 101, EE, 7));

    f = new_fx("order matters A,B vs B,A");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 7));
    fx_add(f, wt(BASE + 5 * MS, 101, WE_A, 1 * MS, 0));
    fx_add(f, wt(BASE + 10 * MS, 101, WE_B, 2 * MS, 0));
    fx_add(f, mk(BASE + 15 * MS, 101, EE, 7));
    fx_add(f, mk(BASE + 20 * MS, 101, ES, 7));
    fx_add(f, wt(BASE + 25 * MS, 101, WE_B, 2 * MS, 0));
    fx_add(f, wt(BASE + 30 * MS, 101, WE_A, 1 * MS, 0));
    fx_add(f, mk(BASE + 35 * MS, 101, EE, 7));

    f = new_fx("loop AAA and pair AA");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 7));
    fx_add(f, wt(BASE + 2 * MS, 101, WE_A, 1 * MS, 0));
    fx_add(f, wt(BASE + 4 * MS, 101, WE_A, 2 * MS, 0));
    fx_add(f, wt(BASE + 6 * MS, 101, WE_A, 3 * MS, 0));
    fx_add(f, mk(BASE + 8 * MS, 101, EE, 7));
    fx_add(f, mk(BASE + 10 * MS, 101, ES, 7));
    fx_add(f, wt(BASE + 12 * MS, 101, WE_A, 4 * MS, 0));
    fx_add(f, wt(BASE + 14 * MS, 101, WE_A, 5 * MS, 0));
    fx_add(f, mk(BASE + 16 * MS, 101, EE, 7));

    f = new_fx("loop body of two ABAB");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 7));
    fx_add(f, wt(BASE + 2 * MS, 101, WE_A, 1 * MS, 0));
    fx_add(f, wt(BASE + 4 * MS, 101, WE_B, 2 * MS, 0));
    fx_add(f, wt(BASE + 6 * MS, 101, WE_A, 3 * MS, 0));
    fx_add(f, wt(BASE + 8 * MS, 101, WE_B, 4 * MS, 0));
    fx_add(f, mk(BASE + 10 * MS, 101, EE, 7));

    f = new_fx("cpu only exec");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 7));
    fx_add(f, mk(BASE + 10 * MS, 101, EE, 7));

    f = new_fx("#222 double EXEC_START");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 7));
    fx_add(f, wt(BASE + 5 * MS, 101, WE_A, 1 * MS, 0));
    fx_add(f, mk(BASE + 10 * MS, 101, ES, 8));
    fx_add(f, wt(BASE + 15 * MS, 101, WE_B, 2 * MS, 0));
    fx_add(f, mk(BASE + 20 * MS, 101, EE, 8));

    f = new_fx("EXEC_END with no start");
    fx_add(f, wt(BASE + 0 * MS, 101, WE_A, 1 * MS, 0));
    fx_add(f, mk(BASE + 5 * MS, 101, EE, 7));
    fx_add(f, wt(BASE + 10 * MS, 101, WE_B, 1 * MS, 0));
    fx_add(f, mk(BASE + 15 * MS, 101, EE, 7));
    f->vacuous_ok = 1;

    f = new_fx("plan and exec interleaved");
    fx_add(f, mk(BASE + 0 * MS, 101, PS, 9));
    fx_add(f, wt(BASE + 2 * MS, 101, WE_A, 1 * MS, 0));
    fx_add(f, mk(BASE + 4 * MS, 101, PE, 9));
    fx_add(f, mk(BASE + 6 * MS, 101, ES, 9));
    fx_add(f, wt(BASE + 8 * MS, 101, WE_B, 2 * MS, 0));
    fx_add(f, wt(BASE + 10 * MS, 101, WE_C, 3 * MS, 0));
    fx_add(f, mk(BASE + 12 * MS, 101, EE, 9));

    f = new_fx("plan enclosing exec");
    fx_add(f, mk(BASE + 0 * MS, 101, PS, 9));
    fx_add(f, wt(BASE + 2 * MS, 101, WE_A, 1 * MS, 0));
    fx_add(f, mk(BASE + 4 * MS, 101, ES, 9));
    fx_add(f, wt(BASE + 6 * MS, 101, WE_B, 2 * MS, 0));
    fx_add(f, mk(BASE + 8 * MS, 101, EE, 9));
    fx_add(f, wt(BASE + 10 * MS, 101, WE_C, 3 * MS, 0));
    fx_add(f, mk(BASE + 12 * MS, 101, PE, 9));

    f = new_fx("two pids interleaved");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 7));
    fx_add(f, mk(BASE + 1 * MS, 202, ES, 8));
    fx_add(f, wt(BASE + 2 * MS, 101, WE_A, 1 * MS, 0));
    fx_add(f, wt(BASE + 3 * MS, 202, WE_B, 2 * MS, 0));
    fx_add(f, wt(BASE + 4 * MS, 101, WE_C, 3 * MS, 0));
    fx_add(f, mk(BASE + 5 * MS, 202, EE, 8));
    fx_add(f, mk(BASE + 6 * MS, 101, EE, 7));

    f = new_fx("idle record inside the sequence");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 7));
    fx_add(f, wt(BASE + 2 * MS, 101, WE_A, 1 * MS, 0));
    fx_add(f, wt(BASE + 4 * MS, 101, WE_IDLE, 50 * MS, 0));
    fx_add(f, wt(BASE + 6 * MS, 101, WE_A, 2 * MS, 0));
    fx_add(f, mk(BASE + 8 * MS, 101, EE, 7));

    f = new_fx("impossible duration record");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 7));
    /* duration_ns > timestamp_ns: pgwt_filter_matches rejects it even for
     * an all-zero filter, so it must not enter the sequence. */
    fx_add(f, wt(BASE + 2 * MS, 101, WE_B, BASE * 2, 0));
    fx_add(f, wt(BASE + 4 * MS, 101, WE_A, 1 * MS, 0));
    fx_add(f, mk(BASE + 6 * MS, 101, EE, 7));

    f = new_fx("no markers at all");
    fx_add(f, wt(BASE + 0 * MS, 101, WE_A, 1 * MS, 0));
    fx_add(f, wt(BASE + 2 * MS, 101, WE_B, 2 * MS, 0));
    fx_add(f, wt(BASE + 4 * MS, 202, WE_C, 3 * MS, 0));
    f->vacuous_ok = 1;

    f = new_fx("query_id only on an inner record");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 0));
    fx_add(f, wt(BASE + 2 * MS, 101, WE_A, 1 * MS, 77));
    fx_add(f, mk(BASE + 4 * MS, 101, EE, 0));

    f = new_fx("query_id on start overwritten inside");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 11));
    fx_add(f, wt(BASE + 2 * MS, 101, WE_A, 1 * MS, 22));
    fx_add(f, mk(BASE + 4 * MS, 101, EE, 11));

    f = new_fx("CMD_END inside an exec");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 7));
    fx_add(f, wt(BASE + 2 * MS, 101, WE_A, 1 * MS, 0));
    fx_add(f, mk(BASE + 4 * MS, 101, CE, 7));
    fx_add(f, wt(BASE + 6 * MS, 101, WE_B, 2 * MS, 0));
    fx_add(f, mk(BASE + 8 * MS, 101, EE, 7));

    f = new_fx("zero duration waits");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 7));
    fx_add(f, wt(BASE + 2 * MS, 101, WE_A, 0, 0));
    fx_add(f, wt(BASE + 4 * MS, 101, WE_B, 0, 0));
    fx_add(f, mk(BASE + 6 * MS, 101, EE, 7));

    f = new_fx("exec open at capture end");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 7));
    fx_add(f, wt(BASE + 2 * MS, 101, WE_A, 1 * MS, 0));
    fx_add(f, wt(BASE + 4 * MS, 101, WE_B, 2 * MS, 0));
    f->vacuous_ok = 1;

    f = new_fx("equal total_ns distinct patterns");
    fx_add(f, mk(BASE + 0 * MS, 101, ES, 7));
    fx_add(f, wt(BASE + 2 * MS, 101, WE_A, 4 * MS, 0));
    fx_add(f, mk(BASE + 4 * MS, 101, EE, 7));
    fx_add(f, mk(BASE + 6 * MS, 101, ES, 7));
    fx_add(f, wt(BASE + 8 * MS, 101, WE_B, 4 * MS, 0));
    fx_add(f, mk(BASE + 10 * MS, 101, EE, 7));

    /* ── heavy fixtures: reduced sweep ── */

    f = new_fx("40 distinct steps (pattern cap)");
    f->heavy = 1;
    t = 0;
    fx_add(f, mk(BASE + (t++) * MS, 101, ES, 7));
    for (int i = 0; i < 40; i++)
        fx_add(f, wt(BASE + (t++) * MS, 101,
                     (PG_WAIT_IO << 24) | (uint32_t)(100 + i),
                     (uint64_t)(i + 1) * MS, 0));
    fx_add(f, mk(BASE + (t++) * MS, 101, EE, 7));

    f = new_fx("150 raw records (raw cap 128)");
    f->heavy = 1;
    t = 0;
    fx_add(f, mk(BASE + (t++) * MS, 101, ES, 7));
    for (int i = 0; i < 150; i++)
        fx_add(f, wt(BASE + (t++) * MS, 101, (i % 3) ? WE_A : WE_B,
                     (uint64_t)(i + 1) * MS, 0));
    fx_add(f, mk(BASE + (t++) * MS, 101, EE, 7));

    f = new_fx("30 execs, varied durations (p95)");
    f->heavy = 1;
    t = 0;
    for (int i = 0; i < 30; i++) {
        fx_add(f, mk(BASE + (t++) * MS, 101, ES, 7));
        fx_add(f, wt(BASE + (t++) * MS, 101, WE_A,
                     (uint64_t)((i * 37) % 23 + 1) * MS, 0));
        fx_add(f, mk(BASE + (t++) * MS, 101, EE, 7));
    }

    f = new_fx("20 distinct query_ids one pattern");
    f->heavy = 1;
    t = 0;
    for (int i = 0; i < 20; i++) {
        fx_add(f, mk(BASE + (t++) * MS, 101, ES, 0));
        fx_add(f, wt(BASE + (t++) * MS, 101, WE_A, 2 * MS,
                     (uint64_t)(1000 + i)));
        fx_add(f, mk(BASE + (t++) * MS, 101, EE, 0));
    }

    f = new_fx("spread sequence, many pids and phases");
    f->heavy = 1;
    t = 0;
    for (int i = 0; i < 12; i++) {
        uint32_t pid = 300 + (uint32_t)(i % 3);
        fx_add(f, mk(BASE + (t++) * MS, pid, PS, (uint64_t)(50 + i % 4)));
        fx_add(f, wt(BASE + (t++) * MS, pid, WE_C, 1 * MS, 0));
        fx_add(f, mk(BASE + (t++) * MS, pid, PE, (uint64_t)(50 + i % 4)));
        fx_add(f, mk(BASE + (t++) * MS, pid, ES, (uint64_t)(50 + i % 4)));
        fx_add(f, wt(BASE + (t++) * MS, pid, WE_A, (uint64_t)(i + 1) * MS, 0));
        fx_add(f, wt(BASE + (t++) * MS, pid, WE_B, 2 * MS, 0));
        fx_add(f, wt(BASE + (t++) * MS, pid, WE_A, (uint64_t)(i + 1) * MS, 0));
        fx_add(f, wt(BASE + (t++) * MS, pid, WE_B, 2 * MS, 0));
        fx_add(f, mk(BASE + (t++) * MS, pid, EE, (uint64_t)(50 + i % 4)));
    }
}

/* ── the oracle, in the server's own input shape ──────────── */

static struct pgwt_trace_event g_oracle_arr[MAXEV];

static void oracle_variants(const struct fixture *f, uint64_t from, uint64_t to,
                            int max_variants, enum pgwt_variant_phase phase,
                            struct pgwt_variants_result *out, int *n_in)
{
    int n = 0;
    /* server_load_events_fi(): every record in [from, to], inclusive. */
    for (int i = 0; i < f->n; i++)
        if (f->ev[i].timestamp_ns >= from && f->ev[i].timestamp_ns <= to)
            g_oracle_arr[n++] = f->ev[i];
    /* handle_variants passes &req->filter, which for an unfiltered request
     * is all-zero but NOT a no-op (it rejects impossible durations). */
    struct pgwt_filter zero;
    memset(&zero, 0, sizeof(zero));
    pgwt_compute_variants(g_oracle_arr, n, &zero, max_variants, phase, out);
    if (n_in) *n_in = n;
}

/* ── the comparator: bit-exact, no epsilon ────────────────── */

static const char *g_field_names[] = {
    "num_variants", "total_executions", "failed",
    "hash", "exec_count", "num_query_ids", "total_ns", "avg_ns",
    "p95_ns", "p95_sample_n", "avg_loop_n", "top_query_id", "num_steps",
    "step.event_id", "step.is_loop", "step.loop_len", "step.name",
    "step_avg_ns",
};
#define F_NUM_VARIANTS 0
#define F_TOTAL_EXECS  1
#define F_FAILED       2
#define F_HASH         3
#define F_EXEC_COUNT   4
#define F_NUM_QIDS     5
#define F_TOTAL_NS     6
#define F_AVG_NS       7
#define F_P95_NS       8
#define F_P95_N        9
#define F_AVG_LOOP_N  10
#define F_TOP_QID     11
#define F_NUM_STEPS   12
#define F_STEP_EVENT  13
#define F_STEP_ISLOOP 14
#define F_STEP_LOOPLEN 15
#define F_STEP_NAME   16
#define F_STEP_AVG    17
#define N_FIELDS      18

/* Returns the index of the first differing field, or -1 when every one is
 * identical. `*row_out` is the variant index at which it differed (-1 for a
 * whole-result field). The avg_loop_n double is compared by its RAW BITS:
 * this is a bit-exactness gate, and == would accept a different encoding of
 * the same value. */
static int variants_diff(const struct pgwt_variants_result *a,
                         const struct pgwt_variants_result *b, int *row_out)
{
    if (row_out) *row_out = -1;
    if (a->num_variants != b->num_variants) return F_NUM_VARIANTS;
    if (a->total_executions != b->total_executions) return F_TOTAL_EXECS;
    if (a->failed != b->failed) return F_FAILED;
    for (int i = 0; i < a->num_variants; i++) {
        const struct pgwt_variant *x = &a->variants[i], *y = &b->variants[i];
        if (row_out) *row_out = i;
        if (x->hash != y->hash) return F_HASH;
        if (x->exec_count != y->exec_count) return F_EXEC_COUNT;
        if (x->num_query_ids != y->num_query_ids) return F_NUM_QIDS;
        if (x->total_ns != y->total_ns) return F_TOTAL_NS;
        if (x->avg_ns != y->avg_ns) return F_AVG_NS;
        if (x->p95_ns != y->p95_ns) return F_P95_NS;
        if (x->p95_sample_n != y->p95_sample_n) return F_P95_N;
        if (memcmp(&x->avg_loop_n, &y->avg_loop_n, sizeof(double)) != 0)
            return F_AVG_LOOP_N;
        if (x->top_query_id != y->top_query_id) return F_TOP_QID;
        if (x->num_steps != y->num_steps) return F_NUM_STEPS;
        for (int s = 0; s < x->num_steps && s < PGWT_MAX_VARIANT_STEPS; s++) {
            if (x->steps[s].event_id != y->steps[s].event_id) return F_STEP_EVENT;
            if (x->steps[s].is_loop != y->steps[s].is_loop) return F_STEP_ISLOOP;
            if (x->steps[s].loop_len != y->steps[s].loop_len) return F_STEP_LOOPLEN;
            if (strcmp(x->steps[s].name, y->steps[s].name) != 0) return F_STEP_NAME;
            if (x->step_avg_ns[s] != y->step_avg_ns[s]) return F_STEP_AVG;
        }
    }
    if (row_out) *row_out = -1;
    return -1;
}

/* ── index construction over an arbitrary cut set ─────────── */

/* Bit i of `cuts` (i in 0..n-2) means "start a new block before event i+1".
 * Pieces are non-empty by construction, so every block's bounds are its own
 * first/last record timestamp — the shape a real committed block has. */
static int build_index(const struct fixture *f, unsigned cuts,
                       struct pgwt_variant_index *idx, int *n_blocks_out)
{
    pgwt_variant_index_init(idx);
    int blk = 0, s = 0;
    for (int e = 1; e <= f->n; e++) {
        int cut = (e == f->n) || ((cuts >> (unsigned)(e - 1)) & 1u);
        if (!cut)
            continue;
        if (pgwt_variant_index_add_block(idx, blk, f->ev[s].timestamp_ns,
                                         f->ev[e - 1].timestamp_ns,
                                         &f->ev[s], e - s) != 0)
            return -1;
        blk++;
        s = e;
    }
    if (n_blocks_out)
        *n_blocks_out = blk;
    return pgwt_variant_index_seal(idx);
}

/* Which block (chunk) a timestamp falls in, or -1. */
static int chunk_of(const struct pgwt_variant_index *idx, uint64_t ts)
{
    for (int c = 0; c < idx->n_chunks; c++)
        if (idx->chunks[c].block_first_ns <= ts &&
            ts <= idx->chunks[c].block_last_ns)
            return c;
    return -1;
}

/* ── non-vacuity ledger ───────────────────────────────────── */

static long c_triples, c_splits, c_variants, c_execs, c_multistep,
            c_looped, c_cpuonly, c_crossing, c_steps_crossing, c_skipped,
            c_multipid, c_qid_from_inner, c_truncated_pattern,
            c_raw_capped, c_p95_sampled, c_two_phase, c_order_pairs,
            c_prefilter_rows_saved;
static long c_mismatch;
static char g_first_mismatch[768];

static void note_mismatch(const struct fixture *f, unsigned cuts,
                          uint64_t from, uint64_t to,
                          enum pgwt_variant_phase phase, int field, int row)
{
    c_mismatch++;
    if (g_first_mismatch[0])
        return;
    snprintf(g_first_mismatch, sizeof(g_first_mismatch),
             "%s cuts=0x%x phase=%s window=[+%llu,+%llu]ms field=%s row=%d",
             f->name, cuts, phase == PGWT_PHASE_EXEC ? "exec" : "plan",
             (unsigned long long)(from - BASE) / MS,
             (unsigned long long)(to - BASE) / MS,
             field >= 0 && field < N_FIELDS ? g_field_names[field] : "?", row);
}

/* Count, from the index itself, the shapes the sweeps claim to exercise.
 * Done against the INDEX so the number reported is the number the
 * carry-over actually had to bridge. */
static void ledger_from_index(const struct pgwt_variant_index *idx,
                              const struct fixture *f, unsigned cuts)
{
    for (int r = 0; r < idx->n_rows; r++) {
        const struct pgwt_variant_index_entry *e = &idx->rows[r];
        int cs = chunk_of(idx, e->start_ns), cc = chunk_of(idx, e->close_ns);
        if (cs >= 0 && cc >= 0 && cs != cc)
            c_crossing++;
        /* The counter that really exercises PARTIAL-SEQUENCE carry-over: an
         * execution whose non-marker records themselves straddle a block
         * boundary. A straddling marker pair with all its waits in one
         * block does not test the partial sequence at all. */
        int first = -1, last = -1;
        for (int i = 0; i < f->n; i++) {
            const struct pgwt_trace_event *ev = &f->ev[i];
            if (ev->pid != e->pid) continue;
            if (ev->timestamp_ns <= e->start_ns || ev->timestamp_ns >= e->close_ns)
                continue;
            if (PGWT_IS_MARKER(ev->old_event) || PGWT_IS_MARKER(ev->new_event))
                continue;
            int ch = chunk_of(idx, ev->timestamp_ns);
            if (first < 0) first = ch;
            last = ch;
        }
        if (first >= 0 && last >= 0 && first != last)
            c_steps_crossing++;
        if (e->raw_len >= PGWT_VARIANT_INDEX_MAX_RAW)
            c_raw_capped++;
        if (e->num_steps >= PGWT_MAX_VARIANT_STEPS)
            c_truncated_pattern++;
    }
    (void)cuts;
}

/* One (fixture, cut set, window, phase) comparison. Returns 0 when the
 * index and the oracle agree bit-exactly, -1 otherwise. A refusal is a
 * failure here: every window these sweeps generate is inside coverage by
 * construction. */
static int compare_one(const struct fixture *f, unsigned cuts,
                       uint64_t from, uint64_t to, enum pgwt_variant_phase phase,
                       int max_variants)
{
    struct pgwt_variant_index idx;
    int n_blocks = 0;
    if (build_index(f, cuts, &idx, &n_blocks) != 0) {
        printf("  FAIL: build refused for %s cuts=0x%x (%s)\n", f->name, cuts,
               pgwt_variant_index_refusal_str(idx.build_refusal));
        pgwt_variant_index_free(&idx);
        return -1;
    }

    struct pgwt_filter zero;
    memset(&zero, 0, sizeof(zero));
    struct pgwt_variant_index_query_result q;
    int rc = pgwt_variant_index_query(&idx, from, to, &zero, max_variants,
                                      phase, &q);

    struct pgwt_variants_result oracle;
    int n_in = 0;
    oracle_variants(f, from, to, max_variants, phase, &oracle, &n_in);

    int bad = 0;
    if (rc != 0) {
        printf("  FAIL: query refused for %s cuts=0x%x window=[+%llu,+%llu] "
               "(%s)\n", f->name, cuts,
               (unsigned long long)(from - BASE) / MS,
               (unsigned long long)(to - BASE) / MS,
               pgwt_variant_index_refusal_str(q.refused));
        bad = 1;
    } else if (oracle.failed) {
        /* The oracle only fails on a real allocation failure, which no
         * fixture here can provoke. Comparing against a failed oracle would
         * be vacuous, so it is a failure, never a skip. */
        printf("  FAIL: ORACLE failed for %s window=[+%llu,+%llu] — "
               "comparison would be vacuous\n", f->name,
               (unsigned long long)(from - BASE) / MS,
               (unsigned long long)(to - BASE) / MS);
        bad = 1;
    } else {
        int row = -1;
        int field = variants_diff(&q.res, &oracle, &row);
        if (field >= 0) {
            note_mismatch(f, cuts, from, to, phase, field, row);
            bad = 1;
        }
        c_triples++;
        c_variants += q.res.num_variants;
        c_execs += q.res.total_executions;
        c_skipped += q.chunks_total - q.chunks_scanned;
        c_prefilter_rows_saved += idx.n_rows - q.rows_examined;
        for (int i = 0; i < q.res.num_variants; i++) {
            const struct pgwt_variant *v = &q.res.variants[i];
            if (v->num_steps > 1) c_multistep++;
            if (v->num_steps == 1 && v->steps[0].event_id == 0) c_cpuonly++;
            if (v->p95_sample_n > 0) c_p95_sampled++;
            for (int s = 0; s < v->num_steps; s++)
                if (v->steps[s].is_loop) { c_looped++; break; }
        }
        /* distinct pids among the selected rows */
        uint32_t seen[8]; int nseen = 0;
        for (int r = 0; r < idx.n_rows && nseen < 8; r++) {
            const struct pgwt_variant_index_entry *e = &idx.rows[r];
            if (e->phase != (uint8_t)phase) continue;
            if (!pgwt_variant_index_selects(e->start_ns, e->close_ns, from, to))
                continue;
            int dup = 0;
            for (int k = 0; k < nseen; k++) if (seen[k] == e->pid) dup = 1;
            if (!dup) seen[nseen++] = e->pid;
        }
        if (nseen > 1) c_multipid++;
        ledger_from_index(&idx, f, cuts);
    }

    pgwt_variant_index_query_free(&q);
    free(oracle.variants);
    pgwt_variant_index_free(&idx);
    return bad ? -1 : 0;
}

/* ── window candidate times ───────────────────────────────── */

static int fx_times(const struct fixture *f, uint64_t *out, int cap)
{
    int n = 0;
    for (int i = 0; i < f->n && n < cap; i++) {
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

/* For a heavy fixture: ~9 evenly spread candidate times instead of all. */
static int fx_times_sparse(const struct fixture *f, uint64_t *out)
{
    uint64_t all[MAXEV];
    int nt = fx_times(f, all, MAXEV);
    int want = nt < 9 ? nt : 9;
    for (int i = 0; i < want; i++)
        out[i] = all[(long)i * (nt - 1) / (want > 1 ? want - 1 : 1)];
    return want;
}

static int g_one_block = 0;      /* PGWT_VARIANT_INDEX_ONE_BLOCK=1 */

/* Cut sets to sweep for a fixture: every one when the fixture is small
 * enough, a spread sample otherwise. In one-block mode, only cuts=0. */
static int cut_sets(const struct fixture *f, unsigned *out, int cap)
{
    if (g_one_block) { out[0] = 0; return 1; }
    int bits = f->n - 1;
    if (bits < 0) bits = 0;
    if (!f->heavy && bits <= 6) {
        int n = 1 << bits;
        if (n > cap) n = cap;
        for (int i = 0; i < n; i++) out[i] = (unsigned)i;
        return n;
    }
    /* A spread sample, always including "no cuts" and "cut everywhere". */
    int n = 0;
    unsigned all = bits >= 31 ? 0x7FFFFFFFu : ((1u << bits) - 1u);
    out[n++] = 0;
    out[n++] = all;
    for (int b = 0; b < bits && n < cap && n < 34; b++)
        out[n++] = 1u << (unsigned)b;
    for (unsigned s = 1; s < 8 && n < cap; s++)
        out[n++] = (0x9E3779B9u * s) & all;
    return n;
}

/* ── sweep A: cut sets x windows x both phases ────────────── */

static void sweep_splits(void)
{
    printf("--- sweep A: block splits x windows x both phases ---\n");
    long bad = 0;
    for (int k = 0; k < g_nfx; k++) {
        const struct fixture *f = &g_fx[k];
        uint64_t T[MAXEV];
        int nt = f->heavy ? fx_times_sparse(f, T) : fx_times(f, T, MAXEV);
        unsigned cuts[64];
        /* 24 cut sets per fixture, not all of them: the oracle allocates a
         * 4 MB hash table per call, so the sweep is paced to a few seconds
         * rather than a minute. Coverage of the shapes that matter is
         * asserted by the ledger, not by the raw quadruple count. */
        int nc = cut_sets(f, cuts, 24);
        for (int ci = 0; ci < nc; ci++) {
            c_splits++;
            for (int i = 0; i < nt; i++)
                for (int j = i; j < nt; j++)
                    for (int p = 0; p < 2; p++)
                        if (compare_one(f, cuts[ci], T[i], T[j],
                                        p ? PGWT_PHASE_PLAN : PGWT_PHASE_EXEC,
                                        20) != 0)
                            bad++;
        }
    }
    CHECK(bad == 0, "sweep A: %ld disagreements over %ld "
          "(fixture,split,window,phase) quadruples", bad, c_triples);
}

/* ── sweep B: window bounds +/- 1 ns (seam inclusivity) ───── */

static void sweep_windows(void)
{
    printf("--- sweep B: window bounds +/- 1 ns (seam inclusivity) ---\n");
    long bad = 0, before = c_triples;
    for (int k = 0; k < g_nfx; k++) {
        const struct fixture *f = &g_fx[k];
        uint64_t T[MAXEV];
        int nt = f->heavy ? fx_times_sparse(f, T) : fx_times(f, T, MAXEV);
        uint64_t lo = T[0], hi = T[nt - 1];
        uint64_t C[MAXEV * 3];
        int ncand = 0;
        for (int i = 0; i < nt; i++) {
            if (T[i] > lo) C[ncand++] = T[i] - 1;
            C[ncand++] = T[i];
            if (T[i] < hi) C[ncand++] = T[i] + 1;
        }
        unsigned cuts[64];
        int nc = cut_sets(f, cuts, 8);
        if (nc > 2) nc = 2;
        for (int ci = 0; ci < nc; ci++)
            for (int i = 0; i < ncand; i++)
                for (int j = i; j < ncand; j++) {
                    if (C[i] < lo || C[j] > hi) continue;
                    for (int p = 0; p < 2; p++)
                        if (compare_one(f, cuts[ci], C[i], C[j],
                                        p ? PGWT_PHASE_PLAN : PGWT_PHASE_EXEC,
                                        20) != 0)
                            bad++;
                }
    }
    CHECK(bad == 0, "sweep B: %ld disagreements over %ld more quadruples",
          bad, c_triples - before);
}

/* ── sweep C: max_variants truncation ────────────────────── */

static void sweep_max_variants(void)
{
    printf("--- sweep C: max_variants truncation (0, 1, 2, 1000) ---\n");
    long bad = 0, before = c_triples;
    const int mv[] = {0, 1, 2, 1000};
    for (int k = 0; k < g_nfx; k++) {
        const struct fixture *f = &g_fx[k];
        uint64_t T[MAXEV];
        int nt = f->heavy ? fx_times_sparse(f, T) : fx_times(f, T, MAXEV);
        unsigned cuts[8];
        int nc = cut_sets(f, cuts, 2);
        for (int ci = 0; ci < nc; ci++)
            for (int m = 0; m < 4; m++)
                for (int p = 0; p < 2; p++)
                    if (compare_one(f, cuts[ci], T[0], T[nt - 1],
                                    p ? PGWT_PHASE_PLAN : PGWT_PHASE_EXEC,
                                    mv[m]) != 0)
                        bad++;
    }
    CHECK(bad == 0, "sweep C: %ld disagreements over %ld more quadruples",
          bad, c_triples - before);
}

/* ── literal expectations for the named adversarial shapes ── */

static struct fixture *fx_by_name(const char *name)
{
    for (int i = 0; i < g_nfx; i++)
        if (strcmp(g_fx[i].name, name) == 0)
            return &g_fx[i];
    return NULL;
}

/* Query a fixture cut into `blocks` equal-ish pieces, over its whole span. */
static int full_query(const char *fixture_name, int blocks,
                      enum pgwt_variant_phase phase,
                      struct pgwt_variant_index *idx,
                      struct pgwt_variant_index_query_result *q)
{
    struct fixture *f = fx_by_name(fixture_name);
    if (!f) { printf("  FAIL: no fixture '%s'\n", fixture_name); return -1; }
    unsigned cuts = 0;
    for (int b = 1; b < blocks; b++) {
        int at = f->n * b / blocks;
        if (at >= 1 && at <= f->n - 1) cuts |= 1u << (unsigned)(at - 1);
    }
    if (build_index(f, cuts, idx, NULL) != 0) {
        printf("  FAIL: build refused for '%s'\n", fixture_name);
        return -1;
    }
    struct pgwt_filter zero;
    memset(&zero, 0, sizeof(zero));
    return pgwt_variant_index_query(idx, f->ev[0].timestamp_ns,
                                    f->ev[f->n - 1].timestamp_ns, &zero, 20,
                                    phase, q);
}

static const struct pgwt_variant *find_hashless(
        const struct pgwt_variant_index_query_result *q, int i)
{
    return (i >= 0 && i < q->res.num_variants) ? &q->res.variants[i] : NULL;
}

static void literal_expectations(void)
{
    printf("--- literal expectations (not a re-derivation of the code) ---\n");
    struct pgwt_variant_index idx;
    struct pgwt_variant_index_query_result q;

    /* one exec, one 5 ms wait, split so the wait is alone in block 2 */
    if (full_query("one exec one wait", 3, PGWT_PHASE_EXEC, &idx, &q) == 0) {
        const struct pgwt_variant *v = find_hashless(&q, 0);
        CHECK(q.res.total_executions == 1, "one exec one wait: 1 execution "
              "(got %d)", q.res.total_executions);
        CHECK(q.res.num_variants == 1, "one exec one wait: 1 variant (got %d)",
              q.res.num_variants);
        CHECK(v && v->num_steps == 1, "one exec one wait: 1 step");
        CHECK(v && v->total_ns == 5 * MS, "one exec one wait: total 5 ms "
              "(got %llu)", v ? (unsigned long long)v->total_ns : 0);
        CHECK(v && v->step_avg_ns[0] == 5 * MS, "one exec one wait: step avg "
              "5 ms");
        CHECK(v && v->exec_count == 1, "one exec one wait: exec_count 1");
        CHECK(v && v->top_query_id == 7, "one exec one wait: query_id 7 from "
              "the start marker");
        CHECK(idx.n_rows == 1, "one exec one wait: exactly 1 index row "
              "(no duplicate across the split) (got %d)", idx.n_rows);
    }
    pgwt_variant_index_query_free(&q);
    pgwt_variant_index_free(&idx);

    /* #222: a second EXEC_START discards the first partial sequence. The
     * oracle never reports it in any window, so neither may the index. */
    if (full_query("#222 double EXEC_START", 3, PGWT_PHASE_EXEC, &idx, &q) == 0) {
        const struct pgwt_variant *v = find_hashless(&q, 0);
        CHECK(q.res.total_executions == 1, "#222: exactly 1 execution, the "
              "first is discarded (got %d)", q.res.total_executions);
        CHECK(idx.n_rows == 1, "#222: exactly 1 index row (got %d)", idx.n_rows);
        CHECK(pgwt_variant_index_discarded(&idx) == 1,
              "#222: 1 partial sequence discarded (got %d)",
              pgwt_variant_index_discarded(&idx));
        CHECK(v && v->num_steps == 1 && v->steps[0].event_id == WE_B,
              "#222: the surviving pattern is [B], not [A] or [A,B]");
        CHECK(v && v->total_ns == 2 * MS, "#222: total is the second "
              "sequence's 2 ms only (got %llu)",
              v ? (unsigned long long)v->total_ns : 0);
    }
    pgwt_variant_index_query_free(&q);
    pgwt_variant_index_free(&idx);

    /* a closing marker with nothing open: ignored, not an error, not a row */
    if (full_query("EXEC_END with no start", 2, PGWT_PHASE_EXEC, &idx, &q) == 0) {
        CHECK(q.refused == PGWT_VARIANT_INDEX_OK && q.res.num_variants == 0 &&
              q.res.total_executions == 0,
              "unbalanced EXEC_END: 0 variants with refused=OK — the one "
              "encoding of a genuinely empty window");
        CHECK(q.sequences_indexed == 1, "unbalanced EXEC_END: still a "
              "complete answer (sequences_indexed=1)");
    }
    pgwt_variant_index_query_free(&q);
    pgwt_variant_index_free(&idx);

    /* a sequence spread across three blocks must be ONE sequence */
    if (full_query("loop body of two ABAB", 5, PGWT_PHASE_EXEC, &idx, &q) == 0) {
        const struct pgwt_variant *v = find_hashless(&q, 0);
        CHECK(idx.n_chunks >= 4, "ABAB: fixture really was cut into >=4 "
              "blocks (got %d)", idx.n_chunks);
        CHECK(v && v->num_steps == 2, "ABAB across blocks: 2 pattern steps, "
              "not 4 and not a truncated 1 (got %d)", v ? v->num_steps : -1);
        CHECK(v && v->steps[0].is_loop && v->steps[0].loop_len == 2,
              "ABAB across blocks: step 0 is a loop of body 2");
        CHECK(v && v->total_ns == 10 * MS, "ABAB across blocks: total 10 ms "
              "(1+2+3+4) (got %llu)", v ? (unsigned long long)v->total_ns : 0);
        CHECK(v && v->avg_loop_n == 2.0, "ABAB across blocks: avg_loop_n "
              "4/2 = 2 (got %f)", v ? v->avg_loop_n : -1);
        /* NOT (1+3)/2 and (2+4)/2. The oracle's per-step walk advances si
         * past the loop step as soon as the NEXT raw record differs, then
         * clamps si to num_steps-1 and keeps accumulating there — so the
         * second A (3 ms) lands nowhere and the second B (4 ms) lands on
         * step 1. step 0 = 1/1 ms, step 1 = (2+4)/2 = 3 ms. This is a
         * quirk of the shipping function, and the index's job is to
         * reproduce it, not to improve on it. */
        CHECK(v && v->step_avg_ns[0] == 1 * MS && v->step_avg_ns[1] == 3 * MS,
              "ABAB across blocks: step avgs 1 ms and 3 ms — the oracle's "
              "si-clamping quirk reproduced, not corrected (got %llu, %llu)",
              v ? (unsigned long long)v->step_avg_ns[0] : 0,
              v ? (unsigned long long)v->step_avg_ns[1] : 0);
    }
    pgwt_variant_index_query_free(&q);
    pgwt_variant_index_free(&idx);

    /* CPU-only: the fill-in happens BEFORE loop_n, so loop_n is 0 */
    if (full_query("cpu only exec", 2, PGWT_PHASE_EXEC, &idx, &q) == 0) {
        const struct pgwt_variant *v = find_hashless(&q, 0);
        CHECK(v && v->num_steps == 1 && v->steps[0].event_id == 0,
              "cpu only: one step, event id 0");
        CHECK(v && strcmp(v->steps[0].name, "CPU*") == 0,
              "cpu only: step name CPU* (got %s)", v ? v->steps[0].name : "?");
        CHECK(v && v->avg_loop_n == 0.0, "cpu only: avg_loop_n 0, because the "
              "CPU fill-in sets raw_len=1 before loop_n is computed (got %f)",
              v ? v->avg_loop_n : -1);
        CHECK(v && v->total_ns == 0, "cpu only: total 0 ns");
    }
    pgwt_variant_index_query_free(&q);
    pgwt_variant_index_free(&idx);

    /* idle records never enter a sequence: [A, idle, A] is [A,A] */
    if (full_query("idle record inside the sequence", 4, PGWT_PHASE_EXEC,
                   &idx, &q) == 0) {
        const struct pgwt_variant *v = find_hashless(&q, 0);
        CHECK(v && v->num_steps == 1 && v->steps[0].is_loop &&
              v->steps[0].loop_len == 1,
              "idle dropped: [A,idle,A] compresses to one A loop, not 3 steps "
              "(got %d steps)", v ? v->num_steps : -1);
        CHECK(v && v->total_ns == 3 * MS, "idle dropped: total 3 ms, the "
              "50 ms ClientRead excluded (got %llu)",
              v ? (unsigned long long)v->total_ns : 0);
    }
    pgwt_variant_index_query_free(&q);
    pgwt_variant_index_free(&idx);

    /* an impossible duration is rejected by the empty filter too */
    if (full_query("impossible duration record", 3, PGWT_PHASE_EXEC,
                   &idx, &q) == 0) {
        const struct pgwt_variant *v = find_hashless(&q, 0);
        CHECK(v && v->num_steps == 1 && v->steps[0].event_id == WE_A,
              "impossible duration: pattern is [A] only — an all-zero filter "
              "still rejects duration > timestamp");
        CHECK(v && v->total_ns == 1 * MS, "impossible duration: total 1 ms, "
              "not 2e12 (got %llu)", v ? (unsigned long long)v->total_ns : 0);
    }
    pgwt_variant_index_query_free(&q);
    pgwt_variant_index_free(&idx);

    /* CMD_END does NOT close an exec for variants (unlike exec_index) */
    if (full_query("CMD_END inside an exec", 4, PGWT_PHASE_EXEC, &idx, &q) == 0) {
        const struct pgwt_variant *v = find_hashless(&q, 0);
        CHECK(q.res.total_executions == 1, "CMD_END inside: still 1 execution");
        CHECK(v && v->num_steps == 2, "CMD_END inside: pattern [A,B] spans the "
              "CMD_END — pgwt_compute_variants ignores it (got %d steps)",
              v ? v->num_steps : -1);
    }
    pgwt_variant_index_query_free(&q);
    pgwt_variant_index_free(&idx);

    /* an execution open at capture end has no row at all */
    if (full_query("exec open at capture end", 3, PGWT_PHASE_EXEC,
                   &idx, &q) == 0) {
        CHECK(q.res.total_executions == 0 && q.res.num_variants == 0,
              "open at capture end: no execution reported");
        CHECK(idx.n_rows == 0, "open at capture end: no index row");
        CHECK(pgwt_variant_index_open_at_end(&idx) == 1,
              "open at capture end: 1 sequence still open (got %d)",
              pgwt_variant_index_open_at_end(&idx));
    }
    pgwt_variant_index_query_free(&q);
    pgwt_variant_index_free(&idx);

    /* ordering is significant: [A,B] and [B,A] are two variants */
    if (full_query("order matters A,B vs B,A", 4, PGWT_PHASE_EXEC,
                   &idx, &q) == 0) {
        CHECK(q.res.num_variants == 2, "order significant: [A,B] and [B,A] are "
              "TWO variants (got %d)", q.res.num_variants);
        CHECK(q.res.total_executions == 2, "order significant: 2 executions");
        if (q.res.num_variants == 2) {
            CHECK(q.res.variants[0].hash != q.res.variants[1].hash,
                  "order significant: the two hashes differ");
            c_order_pairs++;
        }
    }
    pgwt_variant_index_query_free(&q);
    pgwt_variant_index_free(&idx);

    /* query_id picked up from an inner record when the markers carry none */
    if (full_query("query_id only on an inner record", 3, PGWT_PHASE_EXEC,
                   &idx, &q) == 0) {
        const struct pgwt_variant *v = find_hashless(&q, 0);
        CHECK(v && v->top_query_id == 77, "inner query_id 77 reaches the "
              "variant (got %llu)",
              v ? (unsigned long long)v->top_query_id : 0);
        if (v && v->top_query_id == 77) c_qid_from_inner++;
    }
    pgwt_variant_index_query_free(&q);
    pgwt_variant_index_free(&idx);

    /* both phases from ONE index */
    if (full_query("plan and exec interleaved", 4, PGWT_PHASE_PLAN,
                   &idx, &q) == 0) {
        const struct pgwt_variant *v = find_hashless(&q, 0);
        CHECK(q.res.total_executions == 1 && v && v->num_steps == 1 &&
              v->steps[0].event_id == WE_A,
              "plan phase: pattern [A] only");
    }
    pgwt_variant_index_query_free(&q);
    pgwt_variant_index_free(&idx);
    if (full_query("plan and exec interleaved", 4, PGWT_PHASE_EXEC,
                   &idx, &q) == 0) {
        const struct pgwt_variant *v = find_hashless(&q, 0);
        CHECK(q.res.total_executions == 1 && v && v->num_steps == 2 &&
              v->steps[0].event_id == WE_B && v->steps[1].event_id == WE_C,
              "exec phase from the same index: pattern [B,C]");
        c_two_phase++;
    }
    pgwt_variant_index_query_free(&q);
    pgwt_variant_index_free(&idx);
}

/* ── the comparator must see one of everything ────────────── */

static void mutation_probes(void)
{
    printf("--- mutation probes: every compared field must be visible ---\n");
    struct fixture *f = fx_by_name("spread sequence, many pids and phases");
    if (!f) { printf("  FAIL: missing fixture\n"); tests_failed++; return; }

    struct pgwt_variants_result a, b;
    int n_in = 0;
    uint64_t from = f->ev[0].timestamp_ns, to = f->ev[f->n - 1].timestamp_ns;
    oracle_variants(f, from, to, 20, PGWT_PHASE_EXEC, &a, &n_in);
    oracle_variants(f, from, to, 20, PGWT_PHASE_EXEC, &b, &n_in);

    int row = -1;
    CHECK(variants_diff(&a, &b, &row) == -1,
          "probe baseline: two identical results compare equal");
    CHECK(a.num_variants > 0 && a.variants[0].num_steps > 0,
          "probe baseline: the probed result is non-empty (%d variants, %d "
          "steps)", a.num_variants, a.num_variants ? a.variants[0].num_steps : 0);

    int seen[N_FIELDS];
    memset(seen, 0, sizeof(seen));

#define PROBE(field, stmt) do {                                           \
        struct pgwt_variants_result m = b;                                \
        struct pgwt_variant *mv = malloc(sizeof(*mv) *                    \
                                         (b.num_variants ? b.num_variants : 1)); \
        memcpy(mv, b.variants, sizeof(*mv) * (b.num_variants ? b.num_variants : 1)); \
        m.variants = mv;                                                  \
        struct pgwt_variant *v0 = &mv[0];  (void)v0;                      \
        stmt;                                                             \
        int r = -1, got = variants_diff(&a, &m, &r);                      \
        CHECK(got == (field), "probe %s: comparator reports %s",           \
              g_field_names[field],                                        \
              got >= 0 && got < N_FIELDS ? g_field_names[got] : "NOTHING"); \
        if (got == (field)) seen[field] = 1;                              \
        free(mv);                                                         \
    } while (0)

    PROBE(F_TOTAL_EXECS, m.total_executions += 1);
    PROBE(F_FAILED, m.failed = 1);
    PROBE(F_HASH, v0->hash ^= 1u);
    PROBE(F_EXEC_COUNT, v0->exec_count += 1);
    PROBE(F_NUM_QIDS, v0->num_query_ids += 1);
    PROBE(F_AVG_NS, v0->avg_ns += 1);
    PROBE(F_P95_NS, v0->p95_ns += 1);
    PROBE(F_P95_N, v0->p95_sample_n += 1);
    PROBE(F_TOP_QID, v0->top_query_id += 1);
    PROBE(F_STEP_EVENT, v0->steps[0].event_id += 1);
    PROBE(F_STEP_ISLOOP, v0->steps[0].is_loop ^= 1);
    PROBE(F_STEP_LOOPLEN, v0->steps[0].loop_len += 1);
    PROBE(F_STEP_NAME, v0->steps[0].name[0] =
              v0->steps[0].name[0] == 'X' ? 'Y' : 'X');
    PROBE(F_STEP_AVG, v0->step_avg_ns[0] += 1);
    /* total_ns is compared before avg_ns, so perturb it on its own. */
    PROBE(F_TOTAL_NS, v0->total_ns += 1);
    /* num_steps is compared before the per-step fields. */
    PROBE(F_NUM_STEPS, v0->num_steps -= 1);
    /* avg_loop_n by ONE BIT, which an == on doubles would also catch but a
     * tolerance would not — this is the no-epsilon claim, probed. */
    PROBE(F_AVG_LOOP_N, do {
            uint64_t bits; memcpy(&bits, &v0->avg_loop_n, 8); bits ^= 1;
            memcpy(&v0->avg_loop_n, &bits, 8);
          } while (0));
    /* num_variants last: it changes the loop bound. */
    PROBE(F_NUM_VARIANTS, m.num_variants -= 1);
#undef PROBE

    int blind = 0;
    for (int i = 0; i < N_FIELDS; i++)
        if (!seen[i]) { printf("  FAIL: field %s never probed\n",
                               g_field_names[i]); blind++; }
    CHECK(blind == 0, "all %d compared fields are visible to the comparator",
          N_FIELDS);

    free(a.variants);
    free(b.variants);
}

/* ── bypass suite: every way the index could approve blindly ── */

static void expect_refusal(const char *what, struct pgwt_variant_index *idx,
                           uint64_t from, uint64_t to,
                           const struct pgwt_filter *f, int max_variants,
                           enum pgwt_variant_phase phase,
                           enum pgwt_variant_index_refusal want)
{
    struct pgwt_variant_index_query_result q;
    int rc = pgwt_variant_index_query(idx, from, to, f, max_variants, phase, &q);
    int ok = (rc == -1 && q.refused == want && q.res.variants == NULL &&
              q.res.num_variants == PGWT_VARIANT_INDEX_NOT_INDEXED &&
              q.res.total_executions == PGWT_VARIANT_INDEX_NOT_INDEXED &&
              q.res.failed == 1 && q.sequences_indexed == 0);
    CHECK(ok, "%s: REFUSES with %s, no variants, counts poisoned to -1, "
          "failed=1 (got rc=%d refused=%s nv=%d te=%d failed=%d si=%d)",
          what, pgwt_variant_index_refusal_str(want), rc,
          pgwt_variant_index_refusal_str(q.refused), q.res.num_variants,
          q.res.total_executions, q.res.failed, q.sequences_indexed);
    pgwt_variant_index_query_free(&q);
}

static void bypass_suite(void)
{
    printf("--- bypass suite: a gate that cannot see must refuse ---\n");
    struct fixture *f = fx_by_name("plan and exec interleaved");
    uint64_t lo = f->ev[0].timestamp_ns, hi = f->ev[f->n - 1].timestamp_ns;
    struct pgwt_filter zero;
    memset(&zero, 0, sizeof(zero));
    struct pgwt_variant_index idx;

    /* NULL index */
    expect_refusal("NULL index", NULL, lo, hi, &zero, 20, PGWT_PHASE_EXEC,
                   PGWT_VARIANT_INDEX_REFUSE_NULL);

    /* empty index: "no index" must never read as "no executions" */
    pgwt_variant_index_init(&idx);
    pgwt_variant_index_seal(&idx);
    expect_refusal("empty index (no block ever added)", &idx, lo, hi, &zero, 20,
                   PGWT_PHASE_EXEC, PGWT_VARIANT_INDEX_REFUSE_EMPTY);
    pgwt_variant_index_free(&idx);

    /* unsealed */
    pgwt_variant_index_init(&idx);
    pgwt_variant_index_add_block(&idx, 0, lo, hi, f->ev, f->n);
    expect_refusal("unsealed index", &idx, lo, hi, &zero, 20, PGWT_PHASE_EXEC,
                   PGWT_VARIANT_INDEX_REFUSE_UNSEALED);
    pgwt_variant_index_free(&idx);

    /* wrong version */
    if (build_index(f, 0, &idx, NULL) == 0) {
        idx.version = PGWT_VARIANT_INDEX_VERSION + 1;
        expect_refusal("unknown index version", &idx, lo, hi, &zero, 20,
                       PGWT_PHASE_EXEC, PGWT_VARIANT_INDEX_REFUSE_VERSION);
        idx.version = PGWT_VARIANT_INDEX_VERSION;
    }
    pgwt_variant_index_free(&idx);

    /* skipped block in the feed */
    pgwt_variant_index_init(&idx);
    CHECK(pgwt_variant_index_add_block(&idx, 0, f->ev[0].timestamp_ns,
                                       f->ev[1].timestamp_ns, f->ev, 2) == 0,
          "feed: first block accepted");
    CHECK(pgwt_variant_index_add_block(&idx, 2, f->ev[2].timestamp_ns,
                                       f->ev[3].timestamp_ns, &f->ev[2], 2) != 0,
          "feed: a SKIPPED block index is rejected");
    pgwt_variant_index_seal(&idx);
    expect_refusal("skipped block", &idx, lo, hi, &zero, 20, PGWT_PHASE_EXEC,
                   PGWT_VARIANT_INDEX_REFUSE_GAP);
    pgwt_variant_index_free(&idx);

    /* reordered blocks (block bounds going backwards) */
    pgwt_variant_index_init(&idx);
    pgwt_variant_index_add_block(&idx, 0, f->ev[4].timestamp_ns,
                                 f->ev[5].timestamp_ns, &f->ev[4], 2);
    CHECK(pgwt_variant_index_add_block(&idx, 1, f->ev[0].timestamp_ns,
                                       f->ev[1].timestamp_ns, f->ev, 2) != 0,
          "feed: a block whose bounds go BACKWARDS is rejected");
    pgwt_variant_index_seal(&idx);
    expect_refusal("reordered blocks", &idx, lo, hi, &zero, 20, PGWT_PHASE_EXEC,
                   PGWT_VARIANT_INDEX_REFUSE_GAP);
    pgwt_variant_index_free(&idx);

    /* out-of-order record inside a block */
    {
        struct pgwt_trace_event ev[4];
        ev[0] = f->ev[0]; ev[1] = f->ev[1]; ev[2] = f->ev[2]; ev[3] = f->ev[3];
        uint64_t t = ev[1].timestamp_ns;
        ev[1].timestamp_ns = ev[2].timestamp_ns;
        ev[2].timestamp_ns = t;
        pgwt_variant_index_init(&idx);
        CHECK(pgwt_variant_index_add_block(&idx, 0, ev[0].timestamp_ns,
                                           ev[3].timestamp_ns, ev, 4) != 0,
              "feed: a record whose timestamp goes BACKWARDS is rejected, "
              "never silently reordered");
        pgwt_variant_index_seal(&idx);
        expect_refusal("out-of-order record", &idx, lo, hi, &zero, 20,
                       PGWT_PHASE_EXEC, PGWT_VARIANT_INDEX_REFUSE_GAP);
        pgwt_variant_index_free(&idx);
    }

    /* a record outside its own block's header bounds */
    pgwt_variant_index_init(&idx);
    CHECK(pgwt_variant_index_add_block(&idx, 0, f->ev[0].timestamp_ns,
                                       f->ev[1].timestamp_ns, f->ev, 4) != 0,
          "feed: a record outside its block's bounds is rejected, never "
          "clamped");
    pgwt_variant_index_seal(&idx);
    expect_refusal("record outside block bounds", &idx, lo, hi, &zero, 20,
                   PGWT_PHASE_EXEC, PGWT_VARIANT_INDEX_REFUSE_GAP);
    pgwt_variant_index_free(&idx);

    /* add_block after seal */
    if (build_index(f, 0, &idx, NULL) == 0) {
        CHECK(pgwt_variant_index_add_block(&idx, 1, hi, hi, f->ev, 0) != 0,
              "feed: add_block after seal is rejected");
    }
    pgwt_variant_index_free(&idx);

    /* coverage is CONTAINMENT: a window reaching past the last block must
     * refuse, not answer for the part somebody indexed */
    if (build_index(f, 0, &idx, NULL) == 0) {
        expect_refusal("window past cover_to_ns (the 'ends at now' case)",
                       &idx, lo, hi + 1, &zero, 20, PGWT_PHASE_EXEC,
                       PGWT_VARIANT_INDEX_REFUSE_RANGE);
        expect_refusal("window starting before cover_from_ns", &idx, lo - 1, hi,
                       &zero, 20, PGWT_PHASE_EXEC,
                       PGWT_VARIANT_INDEX_REFUSE_RANGE);
        expect_refusal("window entirely outside coverage", &idx, hi + 10,
                       hi + 20, &zero, 20, PGWT_PHASE_EXEC,
                       PGWT_VARIANT_INDEX_REFUSE_RANGE);
        expect_refusal("inverted window", &idx, hi, lo, &zero, 20,
                       PGWT_PHASE_EXEC, PGWT_VARIANT_INDEX_REFUSE_BAD_WINDOW);
        expect_refusal("unknown phase", &idx, lo, hi, &zero, 20,
                       (enum pgwt_variant_phase)7,
                       PGWT_VARIANT_INDEX_REFUSE_PHASE);
        /* the filter gate: NULL is a DIFFERENT oracle, not "no filter" */
        expect_refusal("NULL filter", &idx, lo, hi, NULL, 20, PGWT_PHASE_EXEC,
                       PGWT_VARIANT_INDEX_REFUSE_FILTER);
        struct pgwt_filter ff;
        memset(&ff, 0, sizeof(ff));
        ff.pid = 101;
        expect_refusal("pid filter", &idx, lo, hi, &ff, 20, PGWT_PHASE_EXEC,
                       PGWT_VARIANT_INDEX_REFUSE_FILTER);
        memset(&ff, 0, sizeof(ff));
        ff.event_id = WE_A;
        expect_refusal("event filter", &idx, lo, hi, &ff, 20, PGWT_PHASE_EXEC,
                       PGWT_VARIANT_INDEX_REFUSE_FILTER);
        memset(&ff, 0, sizeof(ff));
        snprintf(ff.class_name, sizeof(ff.class_name), "IO");
        expect_refusal("class filter", &idx, lo, hi, &ff, 20, PGWT_PHASE_EXEC,
                       PGWT_VARIANT_INDEX_REFUSE_FILTER);
        memset(&ff, 0, sizeof(ff));
        ff.query_id = 9;
        expect_refusal("query_id filter", &idx, lo, hi, &ff, 20,
                       PGWT_PHASE_EXEC, PGWT_VARIANT_INDEX_REFUSE_FILTER);
    }
    pgwt_variant_index_free(&idx);

    /* a genuinely empty window: 0 variants with refused=OK, the ONE
     * encoding that is not a refusal */
    {
        struct fixture *nm = fx_by_name("no markers at all");
        if (build_index(nm, 0, &idx, NULL) == 0) {
            struct pgwt_variant_index_query_result q;
            int rc = pgwt_variant_index_query(&idx, nm->ev[0].timestamp_ns,
                                              nm->ev[nm->n - 1].timestamp_ns,
                                              &zero, 20, PGWT_PHASE_EXEC, &q);
            CHECK(rc == 0 && q.refused == PGWT_VARIANT_INDEX_OK &&
                  q.res.num_variants == 0 && q.res.total_executions == 0 &&
                  q.res.failed == 0 && q.sequences_indexed == 1,
                  "fixture with NO markers: 0 variants, refused=OK, failed=0 — "
                  "distinguishable from every refusal above");
            pgwt_variant_index_query_free(&q);
        }
        pgwt_variant_index_free(&idx);
    }

    /* every injected allocation failure must refuse, never trim */
    {
        const char *build_points[] = {
            "variant_index_rows", "variant_index_steps",
            "variant_index_chunks", "variant_index_pids",
            "variant_index_pidix", "pid_index_grow",
        };
        struct fixture *big = fx_by_name("spread sequence, many pids and phases");
        for (unsigned i = 0; i < sizeof(build_points)/sizeof(build_points[0]); i++) {
            setenv("PGWT_TEST_ALLOC_FAIL", build_points[i], 1);
            int built = build_index(big, 0x15, &idx, NULL);
            unsetenv("PGWT_TEST_ALLOC_FAIL");
            char what[128];
            snprintf(what, sizeof(what), "build alloc failure at %s",
                     build_points[i]);
            CHECK(built != 0, "%s: add_block/seal returns failure", what);
            struct pgwt_variant_index_query_result q;
            int rc = pgwt_variant_index_query(&idx, lo, hi, &zero, 20,
                                              PGWT_PHASE_EXEC, &q);
            CHECK(rc == -1 && q.res.failed == 1 && q.sequences_indexed == 0 &&
                  q.res.total_executions == PGWT_VARIANT_INDEX_NOT_INDEXED,
                  "%s: the index is permanently un-queryable (refused=%s)",
                  what, pgwt_variant_index_refusal_str(q.refused));
            pgwt_variant_index_query_free(&q);
            pgwt_variant_index_free(&idx);
        }

        const char *query_points[] = {
            "variant_index_query_ht", "variant_index_samples",
            "variant_index_query_out",
        };
        for (unsigned i = 0; i < sizeof(query_points)/sizeof(query_points[0]); i++) {
            if (build_index(big, 0x15, &idx, NULL) != 0) {
                printf("  FAIL: pristine build failed before query injection\n");
                tests_failed++;
                pgwt_variant_index_free(&idx);
                continue;
            }
            setenv("PGWT_TEST_ALLOC_FAIL", query_points[i], 1);
            char what[128];
            snprintf(what, sizeof(what), "query alloc failure at %s",
                     query_points[i]);
            expect_refusal(what, &idx, big->ev[0].timestamp_ns,
                           big->ev[big->n - 1].timestamp_ns, &zero, 20,
                           PGWT_PHASE_EXEC, PGWT_VARIANT_INDEX_REFUSE_ALLOC);
            unsetenv("PGWT_TEST_ALLOC_FAIL");
            pgwt_variant_index_free(&idx);
        }
    }
}

/* ── the falsifiable prediction, measured ─────────────────── */

/* PREDICTION (stated before the measurement, falsifiable by it):
 *   pgwt_compute_variants is O(events in window) plus a loop-detection pass
 *   per execution; the index query is O(chunks + executions in window x
 *   steps per execution) and touches NO event records at all. So on a trace
 *   with ~50 records per execution the work the query counts
 *   (rows_examined + steps_examined) must be at least 10x smaller than the
 *   number of records the oracle walks, and the prefilter must skip chunks
 *   for a narrow window.
 * The assertion is on COUNTED work, so it cannot pass or fail by machine
 * load. The wall-clock ratio is printed, never asserted. */
static void measure_complexity(void)
{
    printf("--- the falsifiable prediction, measured ---\n");
    const int n_exec = 4000, waits = 48, n_blocks = 40;
    int n = n_exec * (waits + 2);
    struct pgwt_trace_event *ev = calloc((size_t)n, sizeof(*ev));
    if (!ev) { printf("  FAIL: out of memory\n"); tests_failed++; return; }

    uint64_t t = BASE;
    int k = 0;
    for (int e = 0; e < n_exec; e++) {
        uint32_t pid = 1000 + (uint32_t)(e % 64);
        uint64_t qid = 5000 + (uint64_t)(e % 7);
        ev[k++] = mk(t, pid, ES, qid); t += MS;
        for (int w = 0; w < waits; w++) {
            uint32_t wei = (e % 5 == 0) ? WE_A
                         : (e % 5 == 1) ? WE_B
                         : (e % 5 == 2) ? WE_C
                         : (e % 5 == 3) ? WE_D : ((w % 2) ? WE_A : WE_B);
            ev[k++] = wt(t, pid, wei, (uint64_t)(w + 1) * 1000, 0);
            t += MS;
        }
        ev[k++] = mk(t, pid, EE, qid); t += MS;
    }
    n = k;
    uint64_t from = ev[0].timestamp_ns, to = ev[n - 1].timestamp_ns;

    struct pgwt_variant_index idx;
    pgwt_variant_index_init(&idx);
    struct timespec b0, b1;
    clock_gettime(CLOCK_MONOTONIC, &b0);
    int ok = 1;
    for (int b = 0; b < n_blocks; b++) {
        int s = (long)n * b / n_blocks, e2 = (long)n * (b + 1) / n_blocks;
        if (pgwt_variant_index_add_block(&idx, b, ev[s].timestamp_ns,
                                         ev[e2 - 1].timestamp_ns, &ev[s],
                                         e2 - s) != 0) { ok = 0; break; }
    }
    if (ok) ok = (pgwt_variant_index_seal(&idx) == 0);
    clock_gettime(CLOCK_MONOTONIC, &b1);
    CHECK(ok, "big trace: index built over %d blocks, %d records", n_blocks, n);

    struct pgwt_filter zero;
    memset(&zero, 0, sizeof(zero));

    struct pgwt_variant_index_query_result q;
    struct timespec q0, q1;
    clock_gettime(CLOCK_MONOTONIC, &q0);
    int rc = pgwt_variant_index_query(&idx, from, to, &zero, 20,
                                      PGWT_PHASE_EXEC, &q);
    clock_gettime(CLOCK_MONOTONIC, &q1);

    struct pgwt_variants_result oracle;
    struct timespec o0, o1;
    clock_gettime(CLOCK_MONOTONIC, &o0);
    struct pgwt_trace_event *arr = malloc((size_t)n * sizeof(*arr));
    int m = 0;
    for (int i = 0; i < n; i++)
        if (ev[i].timestamp_ns >= from && ev[i].timestamp_ns <= to)
            arr[m++] = ev[i];
    pgwt_compute_variants(arr, m, &zero, 20, PGWT_PHASE_EXEC, &oracle);
    clock_gettime(CLOCK_MONOTONIC, &o1);

    int row = -1;
    CHECK(rc == 0 && !oracle.failed && variants_diff(&q.res, &oracle, &row) == -1,
          "big trace: index == oracle bit-exactly (%d variants, %d executions)",
          q.res.num_variants, q.res.total_executions);

    long work = q.rows_examined + q.steps_examined;
    CHECK(q.res.total_executions == n_exec,
          "big trace: all %d executions selected (got %d)", n_exec,
          q.res.total_executions);
    CHECK(work * 10 <= m, "PREDICTION: query work (rows %ld + steps %ld = "
          "%ld) is >=10x smaller than the %d records the oracle walks",
          q.rows_examined, q.steps_examined, work, m);

    double build_ms = (b1.tv_sec - b0.tv_sec) * 1e3 +
                      (b1.tv_nsec - b0.tv_nsec) / 1e6;
    double qms = (q1.tv_sec - q0.tv_sec) * 1e3 + (q1.tv_nsec - q0.tv_nsec) / 1e6;
    double oms = (o1.tv_sec - o0.tv_sec) * 1e3 + (o1.tv_nsec - o0.tv_nsec) / 1e6;
    printf("  REPORTED (not asserted): %d records / %d executions; "
           "build %.1f ms once; query %.3f ms vs raw %.3f ms = %.1fx\n",
           m, n_exec, build_ms, qms, oms, oms / (qms > 0 ? qms : 1e-9));
    printf("  REPORTED: chunks %ld/%ld scanned, rows_examined %ld, "
           "rows_selected %ld, steps_examined %ld\n",
           q.chunks_scanned, q.chunks_total, q.rows_examined, q.rows_selected,
           q.steps_examined);

    pgwt_variant_index_query_free(&q);
    free(oracle.variants);

    /* the prefilter must actually skip chunks for a narrow window, and the
     * answer must still be exact */
    uint64_t mid_from = ev[n / 2].timestamp_ns, mid_to = ev[n / 2 + 400].timestamp_ns;
    rc = pgwt_variant_index_query(&idx, mid_from, mid_to, &zero, 20,
                                  PGWT_PHASE_EXEC, &q);
    m = 0;
    for (int i = 0; i < n; i++)
        if (ev[i].timestamp_ns >= mid_from && ev[i].timestamp_ns <= mid_to)
            arr[m++] = ev[i];
    pgwt_compute_variants(arr, m, &zero, 20, PGWT_PHASE_EXEC, &oracle);
    CHECK(rc == 0 && !oracle.failed && variants_diff(&q.res, &oracle, &row) == -1,
          "narrow window: index == oracle bit-exactly (%d executions)",
          q.res.total_executions);
    CHECK(q.chunks_scanned < q.chunks_total,
          "narrow window: the prefilter SKIPPED chunks (%ld of %ld scanned)",
          q.chunks_scanned, q.chunks_total);
    CHECK(q.rows_examined < idx.n_rows,
          "narrow window: the prefilter kept %ld of %d rows out of the "
          "selection predicate", (long)(idx.n_rows - q.rows_examined),
          idx.n_rows);
    c_skipped += q.chunks_total - q.chunks_scanned;
    pgwt_variant_index_query_free(&q);
    free(oracle.variants);

    free(arr);
    free(ev);
    pgwt_variant_index_free(&idx);
}

/* ── non-vacuity ledger ───────────────────────────────────── */

struct ledger_row { const char *what; long *val; long min; };

static struct ledger_row g_ledger[] = {
    { "block splits used", &c_splits, 2 },
    { "(fixture,split,window,phase) comparisons", &c_triples, 2000 },
    { "variants actually compared", &c_variants, 100 },
    { "executions actually selected", &c_execs, 100 },
    { "multi-step patterns", &c_multistep, 1 },
    { "patterns containing a LOOP step", &c_looped, 1 },
    { "CPU-only patterns", &c_cpuonly, 1 },
    { "executions whose MARKER PAIR straddled a block boundary", &c_crossing, 1 },
    { "executions whose WAIT RECORDS straddled a block boundary "
      "(partial-sequence carry-over)", &c_steps_crossing, 1 },
    { "patterns truncated at PGWT_MAX_VARIANT_STEPS", &c_truncated_pattern, 1 },
    { "sequences capped at 128 raw records", &c_raw_capped, 1 },
    { "variants with a non-empty p95 sample", &c_p95_sampled, 1 },
    { "windows returning rows for more than one pid", &c_multipid, 1 },
    { "prefilter chunk skips", &c_skipped, 1 },
    { "rows the prefilter kept out of the predicate", &c_prefilter_rows_saved, 1 },
    { "order-sensitive variant pairs proven distinct", &c_order_pairs, 1 },
    { "query_id reaching a variant from an inner record", &c_qid_from_inner, 1 },
    { "both phases answered from one index", &c_two_phase, 1 },
};
#define N_LEDGER ((int)(sizeof(g_ledger)/sizeof(g_ledger[0])))

static void non_vacuity(void)
{
    printf("--- non-vacuity ledger: the sweeps really saw these shapes ---\n");
    for (int i = 0; i < N_LEDGER; i++)
        CHECK(*g_ledger[i].val >= g_ledger[i].min, "%s: %ld (need >= %ld)",
              g_ledger[i].what, *g_ledger[i].val, g_ledger[i].min);
    CHECK(c_mismatch == 0, "no disagreement anywhere%s%s",
          c_mismatch ? " — first: " : "",
          c_mismatch ? g_first_mismatch : "");
}

/* The ledger, falsified. PGWT_VARIANT_INDEX_ONE_BLOCK=1 reruns the sweeps
 * with NO block cuts: the carry-over counters must then be ZERO, which is
 * what proves they are a real gate and not decoration. This mode can never
 * exit 0 — neither outcome is a pass — so it cannot be used to make a red
 * run green. */
static int ledger_falsification(void)
{
    printf("--- LEDGER FALSIFICATION (PGWT_VARIANT_INDEX_ONE_BLOCK=1) ---\n");
    printf("  sweeps ran with cuts=0 only; %ld splits\n", c_splits);
    int zero = (c_crossing == 0 && c_steps_crossing == 0);
    printf("  marker-pair crossings: %ld   wait-record crossings: %ld\n",
           c_crossing, c_steps_crossing);
    if (zero)
        printf("  FALSIFIED AS EXPECTED: with one block per fixture the "
               "carry-over counters are 0, so the ledger's >=1 requirement "
               "is a real gate that a one-block suite would fail.\n");
    else
        printf("  LEDGER IS VACUOUS: carry-over counters are non-zero with no "
               "block cuts at all — the counters measure something else.\n");
    printf("  this mode never exits 0 by design\n");
    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    printf("%s\n", zero ? "FALSIFICATION-OK (not a pass)"
                        : "FALSIFICATION-FAILED");
    return zero ? 2 : 1;
}

int main(void)
{
    const char *ob = getenv("PGWT_VARIANT_INDEX_ONE_BLOCK");
    g_one_block = ob && ob[0] == '1';

    printf("=== test_variant_index: the step-sequence index (Phase 3b) ===\n");
    printf("index version under test: %d\n", PGWT_VARIANT_INDEX_VERSION);
    pgwt_init_event_names(18);
    build_fixtures();
    printf("%d fixtures\n", g_nfx);

    if (g_one_block) {
        sweep_splits();
        sweep_windows();
        return ledger_falsification();
    }

    literal_expectations();
    mutation_probes();
    sweep_splits();
    sweep_windows();
    sweep_max_variants();
    bypass_suite();
    measure_complexity();
    non_vacuity();

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    printf("%s\n", tests_failed ? "FAILED" : "PASSED");
    return tests_failed ? 1 : 0;
}
