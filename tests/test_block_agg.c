/* test_block_agg.c — the per-committed-block transition aggregate
 * (paint-latency Phase 1, src/block_agg.c).
 *
 * WHAT IS BEING PROVED, and against WHAT
 * --------------------------------------
 * The claim of Phase 1 is that merging the aggregates of the blocks wholly
 * inside a window, plus decoding only the partial blocks at each edge, gives
 * BIT-EXACTLY the number the raw path gives. "Bit-exactly" is the whole
 * value: this codebase's failure mode is not crashes, it is numbers that are
 * quietly wrong and change when you resize the window. So there is no epsilon
 * and no tolerance anywhere in this file.
 *
 * The oracle is the SHIPPING raw implementation,
 * pgwt_compute_transitions() in src/compute.c — not a copy of it, and not
 * this module's own accumulator. That matters: if both sides of the
 * comparison came from the same code, the comparison could not fail, which is
 * precisely the defect shape reviewers keep finding here (a conservation check
 * whose two sides came from the same sum). The node-total oracle is written
 * independently in this file (raw_node_oracle()) with its predicate spelled
 * out by hand rather than calling pgwt_block_agg_node_counts().
 *
 * ADVERSARIAL QUESTION — what input makes this pass while the product is
 * broken? Four answers, each with its own pin:
 *   A1. A fixture with one block, or windows that always contain every block.
 *       Then only the MERGE path runs and a double count on the DECODE side is
 *       invisible. PIN: §0's vacuity counters require at least one MERGE, one
 *       DECODE, one SKIP, and at least one window that had a MERGE and a
 *       DECODE block AT THE SAME TIME (the seam).
 *   A2. All durations zero, so every total_ns comparison is 0 == 0. PIN:
 *       distinct prime-ish durations throughout, and §0 requires at least one
 *       compared window with a strictly positive pair total on BOTH sides.
 *   A3. The aggregate silently empty (build failing, predicate rejecting
 *       everything), so every assertion is 0 == 0 again. PIN: §0 requires a
 *       nonzero merged pair count and a nonzero merged transition total; §7's
 *       mutation probes prove the comparator can see one count, one
 *       nanosecond, one extra pair and one missing pair.
 *   A4. The windows all land in gaps, so nothing qualifies. PIN: a
 *       nothing-qualifies window is checked (§8 B16) but is explicitly NOT
 *       counted toward the vacuity budget.
 *
 * WHAT MAKES THIS HANG OR SKIP RATHER THAN FAIL? Nothing waits, polls, sleeps,
 * reads a clock, opens a file, forks, or depends on another tool: every
 * fixture is a literal in this file. The one component whose absence could
 * make the whole thing vacuous is the aggregate itself, which is why §0's
 * counters are asserted at exit and why main() returns nonzero if any counter
 * is unmet even when every individual assertion passed.
 *
 * WHAT DEPENDS ON TIMING OR ORDERING? Only merge order, which is the point of
 * §3: all 6 orderings of 3 block aggregates must produce byte-identical
 * sorted readouts. Nothing depends on wall-clock time. Hash-table iteration
 * order is pinned out by pgwt_block_agg_pairs_sorted()'s total order.
 *
 * SECTIONS
 *   §0 vacuity ledger
 *   §1 the record predicate, differentially against pgwt_compute_transitions
 *   §2 build + merge against literal expected values
 *   §3 merge is associative and commutative
 *   §4 the straddle rule (a pair whose old_event began in an earlier block)
 *   §5 plan() is a partition: exactly one of SKIP/MERGE/DECODE per block
 *   §6 the model test: merged-plus-decoded == raw, over every window
 *   §6b the known divergence from src/server.c's inclusive end bound
 *   §7 mutation probes: the comparator can see one count / one ns / one pair
 *   §8 bypass suite: every way this gate's detection can be made unreachable
 */
#include "block_agg.h"
#include "compute.h"
#include "idle_rule.h"
#include "summary_reader.h"

#include <assert.h>
#include <stddef.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* compute.c's only foreign symbol in this link set (same stub as
 * test_window_clip / test_live_accum). Reaching it would mean a summary path
 * ran, which no test here asks for, so it ABORTS rather than returning
 * quietly: a stub that returned "nothing found" would let a summary path
 * answer this suite's questions from an empty read, which is the exact shape
 * of a gate that approves without having seen anything. */
int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns;
    (void)visitor; (void)ctx;
    fprintf(stderr, "FAIL: no test here may reach the summary path\n");
    abort();
}

static int failures;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fprintf(stderr, "\n");                                         \
        }                                                                  \
    } while (0)

/* ── §0 vacuity ledger ────────────────────────────────────────────────────
 * Every one of these must be nonzero at exit, or the suite failed to exercise
 * the thing it claims to test. A gate that cannot see must refuse. */
static struct {
    long windows_compared;
    long plan_merge;
    long plan_decode;
    long plan_skip;
    long windows_with_merge_and_decode;   /* the seam (A1) */
    long windows_with_positive_total_ns;  /* (A2) */
    long windows_with_nonzero_pairs;      /* (A3) */
    long mutation_probes_went_red;
    long refusals_observed;
} seen;

/* ── fixture plumbing ────────────────────────────────────────────────────── */

#define FIX_MAX_EV 16
#define FIX_MAX_BLOCKS 8

struct fix_block {
    struct pgwt_block_identity id;
    enum pgwt_block_type type;
    int committed;
    struct pgwt_trace_event ev[FIX_MAX_EV];
    int n;
};

static const struct pgwt_trace_identity TRACE_A = {
    .trace_version = 3, .pg_version = 170004,
    .start_time_ns = 1700000000000000000ULL, .clock_offset_ns = 42,
};
static const struct pgwt_trace_identity TRACE_B = {   /* after a rotation */
    .trace_version = 3, .pg_version = 170004,
    .start_time_ns = 1700003600000000000ULL, .clock_offset_ns = 77,
};

/* Event ids. Activity class is hidden (pgwt_is_hidden_event); 0 is CPU. */
#define E_CPU   0u
#define E_IO1   WEI(PG_WAIT_IO, 1)
#define E_IO2   WEI(PG_WAIT_IO, 2)
#define E_LW1   WEI(PG_WAIT_LWLOCK, 1)
#define E_LOCK1 WEI(PG_WAIT_LOCK, 1)
#define E_CLIR  WEI(PG_WAIT_CLIENT, 0)          /* ClientRead: NOT hidden */
#define E_HIDE  WEI(PG_WAIT_ACTIVITY, 3)        /* hidden */

static void ev_set(struct fix_block *b, uint64_t ts, uint32_t pid,
                   uint32_t from, uint32_t to, uint64_t dur, uint32_t flags)
{
    assert(b->n < FIX_MAX_EV);
    struct pgwt_trace_event *e = &b->ev[b->n++];
    memset(e, 0, sizeof(*e));
    e->timestamp_ns = ts;
    e->pid = pid;
    e->old_event = from;
    e->new_event = to;
    e->duration_ns = dur;
    e->flags = flags;
    e->cpu_ns = PGWT_CPU_NS_UNKNOWN;
}

/* Fill the header-derived identity from the records, exactly as the writer
 * does: bounds cover ALL records, contributing or not. */
static void fix_seal(struct fix_block *b, const struct pgwt_trace_identity *tr,
                     uint32_t index, uint64_t file_offset)
{
    b->id.trace = *tr;
    b->id.block_index = index;
    b->id.file_offset = file_offset;
    b->id.num_events = (uint32_t)b->n;
    b->id.first_timestamp_ns = b->n ? b->ev[0].timestamp_ns : 0;
    b->id.last_timestamp_ns = b->n ? b->ev[0].timestamp_ns : 0;
    for (int i = 0; i < b->n; i++) {
        if (b->ev[i].timestamp_ns < b->id.first_timestamp_ns)
            b->id.first_timestamp_ns = b->ev[i].timestamp_ns;
        if (b->ev[i].timestamp_ns > b->id.last_timestamp_ns)
            b->id.last_timestamp_ns = b->ev[i].timestamp_ns;
    }
    b->type = PGWT_BLOCK_TRANSITIONS;
    b->committed = 1;
}

/* ── the raw oracles ─────────────────────────────────────────────────────── */

/* Pairs: the SHIPPING implementation, over the records a raw loader would have
 * admitted for this window. max_rows is far above the fixture's distinct-pair
 * count so no cap can mask a difference. */
#define ORACLE_MAX_ROWS 4096

struct raw_pairs {
    struct pgwt_transitions_result res;
};

static int collect_window(const struct fix_block *blocks, int nb,
                          uint64_t from, uint64_t to, int end_inclusive,
                          struct pgwt_trace_event *out, int max_out)
{
    int n = 0;
    for (int b = 0; b < nb; b++) {
        for (int i = 0; i < blocks[b].n; i++) {
            uint64_t ts = blocks[b].ev[i].timestamp_ns;
            if (ts < from)
                continue;
            if (end_inclusive ? (ts > to) : (ts >= to))
                continue;
            assert(n < max_out);
            out[n++] = blocks[b].ev[i];
        }
    }
    return n;
}

static void raw_pairs_of(struct raw_pairs *rp,
                         const struct pgwt_trace_event *ev, int n)
{
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    pgwt_compute_transitions(ev, n, &f, ORACLE_MAX_ROWS, &rp->res);
}

static void raw_pairs_free(struct raw_pairs *rp) { free(rp->res.rows); }

/* Nodes: an INDEPENDENT implementation. The predicate is written out by hand
 * (not pgwt_block_agg_node_counts()) so a change to the module's predicate is
 * a disagreement here rather than a silent agreement. */
struct node_oracle_row { uint32_t id; uint64_t count, total_ns; };

static int raw_node_oracle(const struct pgwt_trace_event *ev, int n,
                           struct node_oracle_row *out, int max_out)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (ev[i].flags & PGWT_EVENT_FLAG_SAMPLE)
            continue;
        if (WE_CLASS(ev[i].old_event) == PG_WAIT_ACTIVITY)   /* hidden */
            continue;
        if (ev[i].old_event >= 0xFFFFFFF0U && ev[i].old_event <= 0xFFFFFFF7U)
            continue;                                        /* marker */
        int at = -1;
        for (int j = 0; j < k; j++)
            if (out[j].id == ev[i].old_event) { at = j; break; }
        if (at < 0) {
            assert(k < max_out);
            at = k++;
            out[at].id = ev[i].old_event;
            out[at].count = 0;
            out[at].total_ns = 0;
        }
        out[at].count++;
        out[at].total_ns += ev[i].duration_ns;
    }
    return k;
}

/* ── the comparator ──────────────────────────────────────────────────────── */

/* Returns the number of disagreements; 0 means bit-exact. Used by the model
 * test (expects 0) and by the mutation probes (expect > 0), so the comparator
 * itself is proven able to go red rather than assumed to be. */
static int compare_agg_vs_raw(const struct pgwt_block_agg *agg,
                              const struct pgwt_transitions_result *raw,
                              const struct node_oracle_row *nodes, int n_nodes,
                              int verbose)
{
    int bad = 0;

    if (agg->total_transitions != raw->total_transitions) {
        if (verbose)
            fprintf(stderr, "  total_transitions agg=%" PRIu64
                            " raw=%" PRIu64 "\n",
                    agg->total_transitions, raw->total_transitions);
        bad++;
    }
    if (agg->n_pairs != raw->total_rows) {
        if (verbose)
            fprintf(stderr, "  distinct pairs agg=%d raw=%d\n",
                    agg->n_pairs, raw->total_rows);
        bad++;
    }
    /* raw -> agg: every pair the raw path found must be present with the same
     * count AND the same summed duration, to the nanosecond. */
    for (int i = 0; i < raw->num_rows; i++) {
        uint64_t c = 0, t = 0;
        if (!pgwt_block_agg_lookup(agg, raw->rows[i].from_event,
                                   raw->rows[i].to_event, &c, &t)) {
            if (verbose)
                fprintf(stderr, "  pair %u->%u absent from aggregate\n",
                        raw->rows[i].from_event, raw->rows[i].to_event);
            bad++;
            continue;
        }
        if (c != raw->rows[i].count) {
            if (verbose)
                fprintf(stderr, "  pair %u->%u count agg=%" PRIu64
                                " raw=%" PRIu64 "\n",
                        raw->rows[i].from_event, raw->rows[i].to_event,
                        c, raw->rows[i].count);
            bad++;
        }
        /* raw total_ns is a double carrying an integer nanosecond sum; the
         * fixture's values are far below 2^53 so the comparison is exact. */
        if (t != (uint64_t)raw->rows[i].total_ns) {
            if (verbose)
                fprintf(stderr, "  pair %u->%u ns agg=%" PRIu64 " raw=%" PRIu64
                                "\n", raw->rows[i].from_event,
                        raw->rows[i].to_event, t,
                        (uint64_t)raw->rows[i].total_ns);
            bad++;
        }
    }
    /* agg -> raw: and no pair the raw path did NOT find. */
    struct pgwt_block_agg_pair *ap = NULL;
    int an = 0;
    if (pgwt_block_agg_pairs_sorted(agg, &ap, &an) != PGWT_BAGG_OK) {
        if (verbose) fprintf(stderr, "  pairs_sorted refused\n");
        return bad + 1;
    }
    for (int i = 0; i < an; i++) {
        int found = 0;
        for (int j = 0; j < raw->num_rows; j++)
            if (raw->rows[j].from_event == ap[i].from_event &&
                raw->rows[j].to_event == ap[i].to_event) { found = 1; break; }
        if (!found) {
            if (verbose)
                fprintf(stderr, "  pair %u->%u present only in aggregate\n",
                        ap[i].from_event, ap[i].to_event);
            bad++;
        }
    }
    free(ap);

    /* nodes, both directions */
    for (int i = 0; i < n_nodes; i++) {
        uint64_t c = 0, t = 0;
        if (!pgwt_block_agg_node_lookup(agg, nodes[i].id, &c, &t)) {
            if (verbose)
                fprintf(stderr, "  node %u absent from aggregate\n", nodes[i].id);
            bad++;
            continue;
        }
        if (c != nodes[i].count || t != nodes[i].total_ns) {
            if (verbose)
                fprintf(stderr, "  node %u agg=(%" PRIu64 ",%" PRIu64
                                ") raw=(%" PRIu64 ",%" PRIu64 ")\n",
                        nodes[i].id, c, t, nodes[i].count, nodes[i].total_ns);
            bad++;
        }
    }
    if (agg->n_nodes != n_nodes) {
        if (verbose)
            fprintf(stderr, "  node count agg=%d raw=%d\n",
                    agg->n_nodes, n_nodes);
        bad++;
    }
    return bad;
}

/* ── §1 the record predicate, differentially ─────────────────────────────── */

static void section1_predicate_differential(void)
{
    printf("=== §1 record predicate vs pgwt_compute_transitions ===\n");

    struct pgwt_trace_event cases[16];
    const char *names[16];
    int n = 0;

    /* Realistic absolute timestamps. They matter: pgwt_filter_matches()
     * refuses a record whose duration exceeds its own END timestamp, so a
     * fixture with small timestamps and large durations would have the raw
     * path refusing EVERYTHING and the agreement would be vacuous. That is
     * exactly what the first version of this fixture did. */
    #define TS_BASE 1700000000000000000ULL

    #define CASE(nm_, from_, to_, dur_, fl_)                               \
        do {                                                               \
            memset(&cases[n], 0, sizeof(cases[0]));                        \
            cases[n].timestamp_ns = TS_BASE + (uint64_t)n;                 \
            cases[n].pid = 101;                                            \
            cases[n].old_event = (from_);                                  \
            cases[n].new_event = (to_);                                    \
            cases[n].duration_ns = (dur_);                                 \
            cases[n].flags = (fl_);                                        \
            names[n] = (nm_);                                              \
            n++;                                                           \
        } while (0)

    CASE("plain IO->LW",            E_IO1,  E_LW1,  7000, 0);
    CASE("CPU->IO",                 E_CPU,  E_IO1,  3000, 0);
    CASE("ClientRead stays",        E_CLIR, E_CPU,  9000, 0);
    CASE("into ClientRead",         E_CPU,  E_CLIR, 1100, 0);
    CASE("hidden source",           E_HIDE, E_IO1,  5000, 0);
    CASE("hidden target",           E_IO1,  E_HIDE, 5000, 0);
    CASE("exit target",             E_IO1,  PGWT_EVENT_EXIT, 4000, 0);
    CASE("marker source",           PGWT_MARKER_EXEC_START, E_IO1, 0, 0);
    CASE("marker target",           E_IO1, PGWT_MARKER_EXEC_START, 0, 0);
    CASE("sample record",           E_CPU,  E_IO1,  0, PGWT_EVENT_FLAG_SAMPLE);
    CASE("self edge",               E_IO1,  E_IO1,  2200, 0);
    CASE("zero duration pair",      E_LOCK1, E_IO2, 0, 0);
    #undef CASE

    /* The impossible record: duration longer than its own absolute end
     * timestamp. pgwt_filter_matches() refuses it at the raw path's single
     * chokepoint rather than repairing it, so the aggregate must refuse it
     * too. Appended by hand because the macro uses a realistic TS_BASE. */
    memset(&cases[n], 0, sizeof(cases[0]));
    cases[n].timestamp_ns = 1000;
    cases[n].pid = 101;
    cases[n].old_event = E_IO1;
    cases[n].new_event = E_LW1;
    cases[n].duration_ns = 5000;        /* > timestamp_ns: cannot be true */
    names[n] = "duration > own end timestamp";
    n++;

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    int n_counted = 0, n_rejected = 0;
    for (int i = 0; i < n; i++) {
        struct pgwt_transitions_result r;
        pgwt_compute_transitions(&cases[i], 1, &f, ORACLE_MAX_ROWS, &r);
        int raw_counted = (r.total_transitions == 1);
        int mine = pgwt_block_agg_record_counts(&cases[i]);
        CHECK(raw_counted == mine,
              "%s: pgwt_compute_transitions counted=%d, "
              "pgwt_block_agg_record_counts=%d",
              names[i], raw_counted, mine);
        if (raw_counted) n_counted++; else n_rejected++;
        free(r.rows);
    }
    /* "The two predicates agree" is satisfied by both rejecting everything,
     * which is what the first version of this fixture actually did. Both
     * outcomes must be present. */
    CHECK(n_counted >= 5, "only %d of %d cases were COUNTED by the raw path — "
          "an all-rejected fixture makes this agreement vacuous", n_counted, n);
    CHECK(n_rejected >= 5, "only %d of %d cases were REJECTED by the raw path",
          n_rejected, n);
    printf("    %d counted, %d rejected, both predicates agreeing on all %d\n",
           n_counted, n_rejected, n);

    /* The node-side asymmetry for the impossible record, with literals: the
     * raw node pass does not go through pgwt_filter_matches(), so it keeps the
     * record that the link table refuses. */
    CHECK(pgwt_block_agg_record_counts(&cases[n - 1]) == 0,
          "the impossible record must not be a link");
    CHECK(pgwt_block_agg_node_counts(&cases[n - 1]) == 1,
          "the impossible record must still be node time (the raw node pass "
          "does not apply pgwt_filter_matches)");

    /* The node predicate is deliberately WIDER (C5): it keeps a record whose
     * new_event is EXIT or hidden. Pinned with literals so "wider" is a fact
     * and not a hope. */
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.old_event = E_IO1; e.new_event = PGWT_EVENT_EXIT; e.duration_ns = 4000;
    CHECK(pgwt_block_agg_record_counts(&e) == 0, "EXIT must not be a pair");
    CHECK(pgwt_block_agg_node_counts(&e) == 1, "EXIT must still be node time");
    e.new_event = E_HIDE;
    CHECK(pgwt_block_agg_record_counts(&e) == 0, "hidden target: no pair");
    CHECK(pgwt_block_agg_node_counts(&e) == 1, "hidden target: node time kept");
    e.old_event = E_HIDE; e.new_event = E_IO1;
    CHECK(pgwt_block_agg_node_counts(&e) == 0, "hidden source: no node time");
    e.old_event = PGWT_MARKER_EXEC_START;
    CHECK(pgwt_block_agg_node_counts(&e) == 0, "marker source: no node time");
    e.old_event = E_IO1; e.flags = PGWT_EVENT_FLAG_SAMPLE;
    CHECK(pgwt_block_agg_node_counts(&e) == 0, "sample: no node time");

    /* C7 half-open, at the two bounds and one past each. */
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = 500;
    CHECK(pgwt_block_agg_in_window(&e, 500, 600) == 1, "ts == from is in");
    CHECK(pgwt_block_agg_in_window(&e, 501, 600) == 0, "ts < from is out");
    e.timestamp_ns = 600;
    CHECK(pgwt_block_agg_in_window(&e, 500, 600) == 0, "ts == to is OUT");
    e.timestamp_ns = 599;
    CHECK(pgwt_block_agg_in_window(&e, 500, 600) == 1, "ts == to-1 is in");
}

/* ── §2 build + merge against literals ──────────────────────────────────── */

static void section2_literals(void)
{
    printf("=== §2 build + merge against literal expected values ===\n");

    struct fix_block b0, b1;
    memset(&b0, 0, sizeof(b0));
    memset(&b1, 0, sizeof(b1));

    ev_set(&b0, 1000, 11, E_CPU,  E_IO1, 100, 0);
    ev_set(&b0, 1100, 11, E_IO1,  E_CPU, 200, 0);
    ev_set(&b0, 1200, 11, E_CPU,  E_IO1, 400, 0);   /* same pair again */
    ev_set(&b0, 1300, 11, E_HIDE, E_IO1, 800, 0);   /* hidden: neither table */
    fix_seal(&b0, &TRACE_A, 0, 4096);

    ev_set(&b1, 2000, 12, E_CPU, E_IO1, 1000, 0);   /* same pair, next block */
    ev_set(&b1, 2100, 12, E_IO1, PGWT_EVENT_EXIT, 2000, 0); /* node only */
    fix_seal(&b1, &TRACE_A, 1, 8192);

    struct pgwt_block_agg a0, a1, win;
    CHECK(pgwt_block_agg_build(&a0, &b0.id, b0.type, b0.committed,
                               b0.ev, b0.n) == PGWT_BAGG_OK, "build b0");
    CHECK(pgwt_block_agg_build(&a1, &b1.id, b1.type, b1.committed,
                               b1.ev, b1.n) == PGWT_BAGG_OK, "build b1");

    uint64_t c = 0, t = 0;
    CHECK(pgwt_block_agg_lookup(&a0, E_CPU, E_IO1, &c, &t) && c == 2 && t == 500,
          "b0 CPU->IO1 expected (2, 500), got (%" PRIu64 ", %" PRIu64 ")", c, t);
    CHECK(pgwt_block_agg_lookup(&a0, E_IO1, E_CPU, &c, &t) && c == 1 && t == 200,
          "b0 IO1->CPU expected (1, 200), got (%" PRIu64 ", %" PRIu64 ")", c, t);
    CHECK(a0.n_pairs == 2, "b0 distinct pairs expected 2, got %d", a0.n_pairs);
    CHECK(a0.total_transitions == 3, "b0 transitions expected 3, got %" PRIu64,
          a0.total_transitions);
    CHECK(a0.pair_total_ns == 700, "b0 pair ns expected 700, got %" PRIu64,
          a0.pair_total_ns);
    /* nodes: CPU 100+400=500 (2 records), IO1 200 (1). The hidden source is in
     * neither table, so node ns == pair ns here. */
    CHECK(pgwt_block_agg_node_lookup(&a0, E_CPU, &c, &t) && c == 2 && t == 500,
          "b0 node CPU expected (2, 500), got (%" PRIu64 ", %" PRIu64 ")", c, t);
    CHECK(a0.node_total_ns == 700, "b0 node ns expected 700, got %" PRIu64,
          a0.node_total_ns);

    /* b1: the EXIT record is node time but not a pair — C5 wider than C4. */
    CHECK(a1.total_transitions == 1, "b1 transitions expected 1, got %" PRIu64,
          a1.total_transitions);
    CHECK(a1.pair_total_ns == 1000, "b1 pair ns expected 1000, got %" PRIu64,
          a1.pair_total_ns);
    CHECK(a1.node_total_ns == 3000, "b1 node ns expected 3000, got %" PRIu64,
          a1.node_total_ns);
    CHECK(a1.node_total_ns != a1.pair_total_ns,
          "C5 must be strictly wider than C4 on this fixture");

    pgwt_block_agg_init_window(&win);
    CHECK(pgwt_block_agg_merge(&win, &a0) == PGWT_BAGG_OK, "merge a0");
    CHECK(pgwt_block_agg_merge(&win, &a1) == PGWT_BAGG_OK, "merge a1");
    CHECK(pgwt_block_agg_lookup(&win, E_CPU, E_IO1, &c, &t) &&
          c == 3 && t == 1500,
          "merged CPU->IO1 expected (3, 1500), got (%" PRIu64 ", %" PRIu64 ")",
          c, t);
    CHECK(win.total_transitions == 4, "merged transitions expected 4, got %"
          PRIu64, win.total_transitions);
    CHECK(win.pair_total_ns == 1700, "merged pair ns expected 1700, got %"
          PRIu64, win.pair_total_ns);
    CHECK(win.node_total_ns == 3700, "merged node ns expected 3700, got %"
          PRIu64, win.node_total_ns);
    CHECK(win.first_timestamp_ns == 1000 && win.last_timestamp_ns == 2100,
          "merged bounds expected [1000, 2100], got [%" PRIu64 ", %" PRIu64 "]",
          win.first_timestamp_ns, win.last_timestamp_ns);

    /* An absent pair is reported ABSENT and the out params are untouched —
     * never read as a zero (C8). */
    c = 0xdeadbeef; t = 0xdeadbeef;
    CHECK(pgwt_block_agg_lookup(&win, E_LOCK1, E_IO2, &c, &t) == 0,
          "absent pair must report absent");
    CHECK(c == 0xdeadbeef && t == 0xdeadbeef,
          "absent lookup must not write the out params");

    pgwt_block_agg_free(&a0);
    pgwt_block_agg_free(&a1);
    pgwt_block_agg_free(&win);
}

/* ── §3 merge is associative and commutative ────────────────────────────── */

static void section3_merge_algebra(void)
{
    printf("=== §3 merge associativity + commutativity ===\n");

    struct fix_block b[3];
    memset(b, 0, sizeof(b));
    ev_set(&b[0], 1000, 11, E_CPU, E_IO1, 11, 0);
    ev_set(&b[0], 1010, 11, E_IO1, E_LW1, 13, 0);
    fix_seal(&b[0], &TRACE_A, 0, 1024);
    ev_set(&b[1], 2000, 12, E_CPU, E_IO1, 17, 0);
    ev_set(&b[1], 2010, 12, E_LW1, E_CPU, 19, 0);
    fix_seal(&b[1], &TRACE_A, 1, 2048);
    ev_set(&b[2], 3000, 13, E_IO1, E_LW1, 23, 0);
    ev_set(&b[2], 3010, 13, E_LOCK1, E_IO2, 29, 0);
    fix_seal(&b[2], &TRACE_A, 2, 3072);

    struct pgwt_block_agg a[3];
    for (int i = 0; i < 3; i++)
        CHECK(pgwt_block_agg_build(&a[i], &b[i].id, b[i].type, b[i].committed,
                                   b[i].ev, b[i].n) == PGWT_BAGG_OK,
              "build block %d", i);

    static const int perms[6][3] = {
        {0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}
    };
    struct pgwt_block_agg_pair *ref = NULL;
    int ref_n = 0;
    uint64_t ref_total = 0, ref_ns = 0, ref_node_ns = 0;

    for (int p = 0; p < 6; p++) {
        struct pgwt_block_agg win;
        pgwt_block_agg_init_window(&win);
        for (int k = 0; k < 3; k++)
            CHECK(pgwt_block_agg_merge(&win, &a[perms[p][k]]) == PGWT_BAGG_OK,
                  "perm %d merge %d", p, k);
        struct pgwt_block_agg_pair *got = NULL;
        int got_n = 0;
        CHECK(pgwt_block_agg_pairs_sorted(&win, &got, &got_n) == PGWT_BAGG_OK,
              "perm %d readout", p);
        if (p == 0) {
            ref = got; ref_n = got_n;
            ref_total = win.total_transitions;
            ref_ns = win.pair_total_ns;
            ref_node_ns = win.node_total_ns;
            /* CPU->IO1 (twice), IO1->LW1 (twice), LW1->CPU, LOCK1->IO2 */
            CHECK(ref_n == 4, "expected 4 distinct pairs, got %d", ref_n);
            CHECK(ref_total == 6, "expected 6 transitions, got %" PRIu64,
                  ref_total);
            CHECK(ref_ns == 11+13+17+19+23+29,
                  "expected ns 112, got %" PRIu64, ref_ns);
        } else {
            CHECK(got_n == ref_n, "perm %d row count %d != %d", p, got_n, ref_n);
            CHECK(win.total_transitions == ref_total,
                  "perm %d total differs", p);
            CHECK(win.pair_total_ns == ref_ns, "perm %d pair ns differs", p);
            CHECK(win.node_total_ns == ref_node_ns, "perm %d node ns differs", p);
            if (got_n == ref_n)
                CHECK(memcmp(got, ref, (size_t)got_n * sizeof(*got)) == 0,
                      "perm %d sorted readout is not byte-identical to perm 0",
                      p);
            free(got);
        }
        pgwt_block_agg_free(&win);
    }
    free(ref);

    /* Associativity in the other sense: merging the three one at a time must
     * equal merging a pre-merged pair. The window accumulator only absorbs
     * MODE_BLOCK aggregates by design (a window is not a block), so the
     * equivalent statement is that the per-block order above is irrelevant —
     * already asserted — plus: merging the same set into two separate windows
     * and comparing their declared totals. */
    struct pgwt_block_agg w1, w2;
    pgwt_block_agg_init_window(&w1);
    pgwt_block_agg_init_window(&w2);
    pgwt_block_agg_merge(&w1, &a[0]);
    pgwt_block_agg_merge(&w1, &a[1]);
    pgwt_block_agg_merge(&w2, &a[1]);
    pgwt_block_agg_merge(&w2, &a[0]);
    CHECK(w1.total_transitions == w2.total_transitions &&
          w1.pair_total_ns == w2.pair_total_ns &&
          w1.node_total_ns == w2.node_total_ns &&
          w1.n_pairs == w2.n_pairs &&
          w1.first_timestamp_ns == w2.first_timestamp_ns &&
          w1.last_timestamp_ns == w2.last_timestamp_ns,
          "merge is not commutative on the declared totals");
    pgwt_block_agg_free(&w1);
    pgwt_block_agg_free(&w2);
    for (int i = 0; i < 3; i++)
        pgwt_block_agg_free(&a[i]);

    /* §3b the readout's TOTAL order, asserted directly.
     *
     * Comparing two merge orders byte-for-byte (above) is NOT enough to catch
     * a comparator that leaves ties unordered: with few keys the hash layout
     * is the same for every merge order, so both sides come out identically
     * wrong. (Measured: mutation M13 in run_mutations.py — the comparator
     * reduced to count-only — left the permutation comparison GREEN.) So the
     * documented order is asserted as a property, over a fixture with many
     * TIED counts, where hash-slot order and key order differ. */
    struct fix_block tie;
    memset(&tie, 0, sizeof(tie));
    static const uint32_t froms[] = { E_IO2, E_CPU, E_LW1, E_LOCK1, E_IO1,
                                      E_CLIR };
    static const uint32_t tos[]   = { E_CPU, E_IO1 };
    uint64_t ts = 100000;
    for (size_t fi = 0; fi < sizeof(froms) / sizeof(froms[0]); fi++)
        for (size_t ti = 0; ti < sizeof(tos) / sizeof(tos[0]); ti++) {
            if (froms[fi] == tos[ti])
                continue;                       /* keep every count at 1 */
            ev_set(&tie, ts, 21, froms[fi], tos[ti], 1, 0);
            ts += 100;
        }
    fix_seal(&tie, &TRACE_A, 5, 6144);
    struct pgwt_block_agg at, wt;
    CHECK(pgwt_block_agg_build(&at, &tie.id, tie.type, 1, tie.ev, tie.n)
          == PGWT_BAGG_OK, "§3b build");
    pgwt_block_agg_init_window(&wt);
    CHECK(pgwt_block_agg_merge(&wt, &at) == PGWT_BAGG_OK, "§3b merge");
    struct pgwt_block_agg_pair *sp = NULL;
    int spn = 0;
    CHECK(pgwt_block_agg_pairs_sorted(&wt, &sp, &spn) == PGWT_BAGG_OK,
          "§3b readout");
    CHECK(spn >= 10, "§3b needs >= 10 tied pairs to be able to detect an "
          "unordered tie, got %d", spn);
    int ties = 0;
    for (int i = 0; i + 1 < spn; i++) {
        if (sp[i].count == sp[i + 1].count)
            ties++;
        int ordered =
            (sp[i].count > sp[i + 1].count) ||
            (sp[i].count == sp[i + 1].count &&
             (sp[i].from_event < sp[i + 1].from_event ||
              (sp[i].from_event == sp[i + 1].from_event &&
               sp[i].to_event < sp[i + 1].to_event)));
        CHECK(ordered,
              "§3b rows %d/%d violate the documented total order "
              "(count desc, from asc, to asc): (%u->%u, %" PRIu64 ") then "
              "(%u->%u, %" PRIu64 ")",
              i, i + 1, sp[i].from_event, sp[i].to_event, sp[i].count,
              sp[i + 1].from_event, sp[i + 1].to_event, sp[i + 1].count);
    }
    CHECK(ties >= 9, "§3b saw only %d tied adjacent pairs — without ties this "
          "check cannot see an unordered comparator", ties);
    free(sp);

    struct pgwt_block_agg_node *sn = NULL;
    int snn = 0;
    CHECK(pgwt_block_agg_nodes_sorted(&wt, &sn, &snn) == PGWT_BAGG_OK,
          "§3b node readout");
    for (int i = 0; i + 1 < snn; i++)
        CHECK(sn[i].total_ns > sn[i + 1].total_ns ||
              (sn[i].total_ns == sn[i + 1].total_ns &&
               sn[i].event_id < sn[i + 1].event_id),
              "§3b node rows %d/%d violate (total_ns desc, event_id asc)",
              i, i + 1);
    free(sn);
    pgwt_block_agg_free(&wt);
    pgwt_block_agg_free(&at);
}

/* ── §4 the straddle rule ───────────────────────────────────────────────── */

static void section4_straddle(void)
{
    printf("=== §4 straddle: a pair whose old_event began in an earlier block "
           "===\n");

    /* One pid's logical sequence CPU -> IO1 -> LW1 -> CPU, cut by a block
     * boundary in the middle. The record at ts 2000 closes an IO1 interval
     * that started at ts 1000, i.e. in the PREVIOUS block. The trace stores
     * one record per transition, so that pair lives in exactly one block.
     *
     * The decided rule (C6): counted ONCE, in the block holding the record,
     * with its duration attributed IN FULL and unclipped. Not twice, not lost,
     * not split. The literals below are the whole statement of that rule. */
    struct fix_block b0, b1;
    memset(&b0, 0, sizeof(b0));
    memset(&b1, 0, sizeof(b1));

    ev_set(&b0, 1000, 11, E_CPU, E_IO1, 300, 0);   /* enters IO1 at ts 1000 */
    fix_seal(&b0, &TRACE_A, 0, 1024);
    ev_set(&b1, 2000, 11, E_IO1, E_LW1, 1000, 0);  /* leaves IO1 — 1000 ns,
                                                    * all of it before b1 */
    ev_set(&b1, 2500, 11, E_LW1, E_CPU, 500, 0);
    fix_seal(&b1, &TRACE_A, 1, 2048);

    struct pgwt_block_agg a0, a1, win;
    CHECK(pgwt_block_agg_build(&a0, &b0.id, b0.type, 1, b0.ev, b0.n)
          == PGWT_BAGG_OK, "straddle build b0");
    CHECK(pgwt_block_agg_build(&a1, &b1.id, b1.type, 1, b1.ev, b1.n)
          == PGWT_BAGG_OK, "straddle build b1");

    uint64_t c = 0, t = 0;
    CHECK(pgwt_block_agg_lookup(&a0, E_IO1, E_LW1, &c, &t) == 0,
          "the straddling pair must NOT appear in the earlier block");
    CHECK(pgwt_block_agg_lookup(&a1, E_IO1, E_LW1, &c, &t) && c == 1 &&
          t == 1000,
          "the straddling pair belongs to the block holding the record, "
          "with its FULL duration: expected (1, 1000), got (%" PRIu64
          ", %" PRIu64 ")", c, t);

    pgwt_block_agg_init_window(&win);
    pgwt_block_agg_merge(&win, &a0);
    pgwt_block_agg_merge(&win, &a1);
    CHECK(pgwt_block_agg_lookup(&win, E_IO1, E_LW1, &c, &t) && c == 1 &&
          t == 1000, "merged: counted exactly ONCE, not twice");
    CHECK(win.total_transitions == 3,
          "merged transitions expected 3, got %" PRIu64, win.total_transitions);

    /* And the same against the raw oracle over both blocks' records. */
    struct pgwt_trace_event all[FIX_MAX_EV * 2];
    struct fix_block pair_blocks[2] = { b0, b1 };
    int n = collect_window(pair_blocks, 2, 0, UINT64_MAX, 0, all,
                           (int)(sizeof(all) / sizeof(all[0])));
    struct raw_pairs rp;
    raw_pairs_of(&rp, all, n);
    struct node_oracle_row nodes[16];
    int nn = raw_node_oracle(all, n, nodes, 16);
    int bad = compare_agg_vs_raw(&win, &rp.res, nodes, nn, 1);
    CHECK(bad == 0, "straddle fixture: %d disagreement(s) with the raw path",
          bad);
    raw_pairs_free(&rp);

    pgwt_block_agg_free(&a0);
    pgwt_block_agg_free(&a1);
    pgwt_block_agg_free(&win);
}

/* ── the model fixture, shared by §5, §6, §6b, §7 ───────────────────────── */

static int build_model_fixture(struct fix_block *b, int max_blocks)
{
    assert(max_blocks >= 5);
    memset(b, 0, sizeof(*b) * (size_t)max_blocks);
    int nb = 0;

    /* Block 0: ordinary traffic, a repeated pair, a hidden endpoint. */
    ev_set(&b[nb], 1000, 11, E_CPU,  E_IO1,  101, 0);
    ev_set(&b[nb], 1100, 11, E_IO1,  E_CPU,  103, 0);
    ev_set(&b[nb], 1200, 12, E_CPU,  E_IO1,  107, 0);
    ev_set(&b[nb], 1300, 12, E_IO1,  E_HIDE, 109, 0);   /* node only */
    ev_set(&b[nb], 1400, 11, E_HIDE, E_CPU,  113, 0);   /* neither */
    fix_seal(&b[nb], &TRACE_A, 0, 1024); nb++;

    /* Block 1: markers, a sample record, an EXIT, a self edge. */
    ev_set(&b[nb], 2000, 11, PGWT_MARKER_EXEC_START, PGWT_MARKER_EXEC_START,
           0, 0);
    ev_set(&b[nb], 2100, 11, E_CPU,  E_IO1,  127, 0);
    ev_set(&b[nb], 2200, 11, E_CPU,  E_IO1,    0, PGWT_EVENT_FLAG_SAMPLE);
    ev_set(&b[nb], 2300, 13, E_IO1,  PGWT_EVENT_EXIT, 131, 0);
    ev_set(&b[nb], 2400, 13, E_LW1,  E_LW1,  137, 0);   /* self edge counts */
    fix_seal(&b[nb], &TRACE_A, 1, 2048); nb++;

    /* Block 2: ClientRead both ways (visible, must count), and lock traffic. */
    ev_set(&b[nb], 3000, 14, E_CLIR,  E_CPU,  139, 0);
    ev_set(&b[nb], 3100, 14, E_CPU,   E_CLIR, 149, 0);
    ev_set(&b[nb], 3200, 14, E_LOCK1, E_IO2,  151, 0);
    ev_set(&b[nb], 3300, 15, E_IO2,   E_LOCK1,157, 0);
    fix_seal(&b[nb], &TRACE_A, 2, 3072); nb++;

    /* Block 3: a single record, so a window can contain it wholly or not at
     * all with nothing else in the way. */
    ev_set(&b[nb], 4000, 16, E_CPU, E_IO1, 163, 0);
    fix_seal(&b[nb], &TRACE_A, 3, 4096); nb++;

    /* Block 4: dense, same timestamp twice (two pids transitioning at the
     * same instant), and the pairs already seen so merging must SUM. */
    ev_set(&b[nb], 5000, 17, E_IO1, E_CPU, 167, 0);
    ev_set(&b[nb], 5000, 18, E_IO1, E_CPU, 173, 0);
    ev_set(&b[nb], 5100, 17, E_CPU, E_IO1, 179, 0);
    ev_set(&b[nb], 5200, 18, E_LW1, E_LW1, 181, 0);
    fix_seal(&b[nb], &TRACE_A, 4, 5120); nb++;

    return nb;
}

/* Every interesting window boundary: each block's bounds and one past each,
 * plus every record timestamp. Deliberately includes values that make a block
 * MERGE, DECODE and SKIP. */
static int model_boundaries(const struct fix_block *b, int nb,
                            uint64_t *out, int max_out)
{
    int n = 0;
    for (int i = 0; i < nb; i++) {
        uint64_t v[4] = { b[i].id.first_timestamp_ns - 1,
                          b[i].id.first_timestamp_ns,
                          b[i].id.last_timestamp_ns,
                          b[i].id.last_timestamp_ns + 1 };
        for (int k = 0; k < 4; k++) {
            int dup = 0;
            for (int j = 0; j < n; j++) if (out[j] == v[k]) { dup = 1; break; }
            if (!dup) { assert(n < max_out); out[n++] = v[k]; }
        }
        for (int e = 0; e < b[i].n; e++) {
            uint64_t ts = b[i].ev[e].timestamp_ns;
            int dup = 0;
            for (int j = 0; j < n; j++) if (out[j] == ts) { dup = 1; break; }
            if (!dup) { assert(n < max_out); out[n++] = ts; }
        }
    }
    /* ascending, so window enumeration is deterministic */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && out[j - 1] > out[j]; j--) {
            uint64_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return n;
}

/* Build the window answer the Phase-1 way: merge the aggregates of the blocks
 * the plan calls MERGE, and add the in-window records of the blocks the plan
 * calls DECODE. This is the function the whole phase stands on. */
static int answer_window_phase1(const struct fix_block *b, int nb,
                                const struct pgwt_block_agg *per_block,
                                const int *have_agg,
                                uint64_t from, uint64_t to,
                                struct pgwt_block_agg *out,
                                int *n_merge, int *n_decode, int *n_skip)
{
    pgwt_block_agg_init_window(out);
    *n_merge = *n_decode = *n_skip = 0;
    for (int i = 0; i < nb; i++) {
        enum pgwt_block_plan p =
            pgwt_block_agg_plan(&b[i].id, have_agg[i], from, to);
        if (p == PGWT_BLOCK_SKIP) {
            (*n_skip)++;
            continue;
        }
        if (p == PGWT_BLOCK_MERGE) {
            (*n_merge)++;
            int rc = pgwt_block_agg_merge(out, &per_block[i]);
            if (rc != PGWT_BAGG_OK)
                return rc;
            continue;
        }
        (*n_decode)++;
        for (int e = 0; e < b[i].n; e++) {
            if (!pgwt_block_agg_in_window(&b[i].ev[e], from, to))
                continue;
            int rc = pgwt_block_agg_add_event(out, &b[i].ev[e]);
            if (rc != PGWT_BAGG_OK)
                return rc;
        }
    }
    return PGWT_BAGG_OK;
}

/* ── §5 plan() is a partition ───────────────────────────────────────────── */

static void section5_plan_partition(void)
{
    printf("=== §5 plan(): exactly one of SKIP/MERGE/DECODE per block ===\n");

    struct fix_block b[FIX_MAX_BLOCKS];
    int nb = build_model_fixture(b, FIX_MAX_BLOCKS);
    uint64_t bounds[128];
    int nbnd = model_boundaries(b, nb, bounds, 128);

    long merge = 0, decode = 0, skip = 0;
    for (int i = 0; i < nbnd; i++) {
        for (int j = i + 1; j < nbnd; j++) {
            uint64_t from = bounds[i], to = bounds[j];
            for (int k = 0; k < nb; k++) {
                enum pgwt_block_plan p =
                    pgwt_block_agg_plan(&b[k].id, 1, from, to);
                CHECK(p == PGWT_BLOCK_SKIP || p == PGWT_BLOCK_MERGE ||
                      p == PGWT_BLOCK_DECODE,
                      "plan returned %d, not one of the three", (int)p);
                if (p == PGWT_BLOCK_MERGE) {
                    merge++;
                    /* A MERGEd block must be wholly inside under the half-open
                     * rule — stated here independently of plan()'s own code. */
                    CHECK(b[k].id.first_timestamp_ns >= from &&
                          b[k].id.last_timestamp_ns < to,
                          "MERGE of a block not wholly inside [%" PRIu64
                          ", %" PRIu64 ")", from, to);
                    /* ... and therefore every one of its records is in-window,
                     * which is what makes folding the whole aggregate sound. */
                    for (int e = 0; e < b[k].n; e++)
                        CHECK(pgwt_block_agg_in_window(&b[k].ev[e], from, to),
                              "MERGEd block holds an out-of-window record");
                } else if (p == PGWT_BLOCK_SKIP) {
                    skip++;
                    /* A SKIPped block must hold NO in-window record. */
                    for (int e = 0; e < b[k].n; e++)
                        CHECK(!pgwt_block_agg_in_window(&b[k].ev[e], from, to),
                              "SKIPped block holds an in-window record "
                              "(ts %" PRIu64 ", window [%" PRIu64 ", %" PRIu64
                              "))", b[k].ev[e].timestamp_ns, from, to);
                } else {
                    decode++;
                }
            }
        }
    }
    CHECK(merge > 0 && decode > 0 && skip > 0,
          "plan coverage: merge=%ld decode=%ld skip=%ld — all three must occur",
          merge, decode, skip);
    printf("    plan outcomes over %d windows: merge=%ld decode=%ld skip=%ld\n",
           nbnd * (nbnd - 1) / 2, merge, decode, skip);

    /* No aggregate => DECODE, never MERGE (C8), on exactly the windows that
     * would otherwise have merged. */
    long would_merge = 0;
    for (int i = 0; i < nbnd; i++)
        for (int j = i + 1; j < nbnd; j++)
            for (int k = 0; k < nb; k++)
                if (pgwt_block_agg_plan(&b[k].id, 1, bounds[i], bounds[j])
                    == PGWT_BLOCK_MERGE) {
                    would_merge++;
                    CHECK(pgwt_block_agg_plan(&b[k].id, 0, bounds[i],
                                              bounds[j]) == PGWT_BLOCK_DECODE,
                          "have_agg=0 must give DECODE, not MERGE");
                }
    CHECK(would_merge == merge, "have_agg sweep saw %ld, expected %ld",
          would_merge, merge);
}

/* ── §6 the model test ──────────────────────────────────────────────────── */

static void section6_model(void)
{
    printf("=== §6 model: merged + decoded == raw, bit-exact, every window "
           "===\n");

    struct fix_block b[FIX_MAX_BLOCKS];
    int nb = build_model_fixture(b, FIX_MAX_BLOCKS);

    struct pgwt_block_agg per_block[FIX_MAX_BLOCKS];
    int have_agg[FIX_MAX_BLOCKS];
    for (int i = 0; i < nb; i++) {
        int rc = pgwt_block_agg_build(&per_block[i], &b[i].id, b[i].type,
                                      b[i].committed, b[i].ev, b[i].n);
        CHECK(rc == PGWT_BAGG_OK, "model build block %d rc=%d", i, rc);
        have_agg[i] = (rc == PGWT_BAGG_OK);
        CHECK(pgwt_block_agg_verify_payload(&per_block[i], b[i].ev, b[i].n),
              "payload hash must verify against the records it was built from");
    }

    uint64_t bounds[128];
    int nbnd = model_boundaries(b, nb, bounds, 128);

    for (int i = 0; i < nbnd; i++) {
        for (int j = i + 1; j < nbnd; j++) {
            uint64_t from = bounds[i], to = bounds[j];

            struct pgwt_block_agg win;
            int nm, nd, ns;
            int rc = answer_window_phase1(b, nb, per_block, have_agg, from, to,
                                          &win, &nm, &nd, &ns);
            CHECK(rc == PGWT_BAGG_OK, "phase1 answer refused rc=%d", rc);

            struct pgwt_trace_event raw[FIX_MAX_EV * FIX_MAX_BLOCKS];
            int n = collect_window(b, nb, from, to, 0, raw,
                                   (int)(sizeof(raw) / sizeof(raw[0])));
            struct raw_pairs rp;
            raw_pairs_of(&rp, raw, n);
            struct node_oracle_row nodes[32];
            int nn = raw_node_oracle(raw, n, nodes, 32);

            int bad = compare_agg_vs_raw(&win, &rp.res, nodes, nn, 0);
            if (bad) {
                fprintf(stderr, "window [%" PRIu64 ", %" PRIu64 ") "
                        "merge=%d decode=%d skip=%d:\n", from, to, nm, nd, ns);
                compare_agg_vs_raw(&win, &rp.res, nodes, nn, 1);
            }
            CHECK(bad == 0, "window [%" PRIu64 ", %" PRIu64 "): %d "
                  "disagreement(s) with the raw path", from, to, bad);

            seen.windows_compared++;
            seen.plan_merge += nm;
            seen.plan_decode += nd;
            seen.plan_skip += ns;
            if (nm > 0 && nd > 0)
                seen.windows_with_merge_and_decode++;
            if (win.pair_total_ns > 0 && rp.res.total_transitions > 0)
                seen.windows_with_positive_total_ns++;
            if (win.n_pairs > 0)
                seen.windows_with_nonzero_pairs++;

            raw_pairs_free(&rp);
            pgwt_block_agg_free(&win);
        }
    }
    printf("    %ld windows compared, %ld of them at a merge/decode seam\n",
           seen.windows_compared, seen.windows_with_merge_and_decode);

    for (int i = 0; i < nb; i++)
        pgwt_block_agg_free(&per_block[i]);
}

/* ── §6b the known divergence from the loader's inclusive end bound ─────── */

static void section6b_inclusive_end_divergence(void)
{
    printf("=== §6b the loader's inclusive end bound: the divergence, "
           "with a literal ===\n");

    /* src/server.c admits an exact record when ts <= to_m. This module is
     * half-open. The difference is exactly the records at ts == to — not
     * "approximately", not "within tolerance". Pinned so the wiring step
     * cannot forget to reconcile the loader (see block_agg.h). */
    struct fix_block b[FIX_MAX_BLOCKS];
    int nb = build_model_fixture(b, FIX_MAX_BLOCKS);
    struct pgwt_block_agg per_block[FIX_MAX_BLOCKS];
    int have_agg[FIX_MAX_BLOCKS];
    for (int i = 0; i < nb; i++) {
        have_agg[i] = pgwt_block_agg_build(&per_block[i], &b[i].id, b[i].type,
                                           b[i].committed, b[i].ev, b[i].n)
                      == PGWT_BAGG_OK;
        CHECK(have_agg[i], "6b build %d", i);
    }

    /* to == 4000, which is block 3's single record. Half-open excludes it;
     * an inclusive end bound includes it. */
    uint64_t from = 1000, to = 4000;
    struct pgwt_block_agg win;
    int nm, nd, ns;
    CHECK(answer_window_phase1(b, nb, per_block, have_agg, from, to, &win,
                               &nm, &nd, &ns) == PGWT_BAGG_OK, "6b answer");

    struct pgwt_trace_event raw_incl[FIX_MAX_EV * FIX_MAX_BLOCKS];
    int n_incl = collect_window(b, nb, from, to, 1, raw_incl,
                                (int)(sizeof(raw_incl) /
                                      sizeof(raw_incl[0])));
    struct raw_pairs rp_incl;
    raw_pairs_of(&rp_incl, raw_incl, n_incl);

    /* The record at ts 4000 is CPU->IO1 with duration 163. */
    CHECK(rp_incl.res.total_transitions == win.total_transitions + 1,
          "end-inclusive raw must count exactly ONE more transition than the "
          "half-open merge here: raw=%" PRIu64 " merged=%" PRIu64,
          rp_incl.res.total_transitions, win.total_transitions);

    uint64_t c_merged = 0, t_merged = 0;
    CHECK(pgwt_block_agg_lookup(&win, E_CPU, E_IO1, &c_merged, &t_merged),
          "6b CPU->IO1 present");
    uint64_t c_raw = 0, t_raw = 0;
    for (int i = 0; i < rp_incl.res.num_rows; i++)
        if (rp_incl.res.rows[i].from_event == E_CPU &&
            rp_incl.res.rows[i].to_event == E_IO1) {
            c_raw = rp_incl.res.rows[i].count;
            t_raw = (uint64_t)rp_incl.res.rows[i].total_ns;
        }
    CHECK(c_raw == c_merged + 1 && t_raw == t_merged + 163,
          "the divergence must be exactly the ts == to record (163 ns): "
          "raw=(%" PRIu64 ", %" PRIu64 ") merged=(%" PRIu64 ", %" PRIu64 ")",
          c_raw, t_raw, c_merged, t_merged);

    /* And with the matching half-open raw predicate, zero divergence. */
    struct pgwt_trace_event raw_ho[FIX_MAX_EV * FIX_MAX_BLOCKS];
    int n_ho = collect_window(b, nb, from, to, 0, raw_ho,
                              (int)(sizeof(raw_ho) / sizeof(raw_ho[0])));
    struct raw_pairs rp_ho;
    raw_pairs_of(&rp_ho, raw_ho, n_ho);
    struct node_oracle_row nodes[32];
    int nn = raw_node_oracle(raw_ho, n_ho, nodes, 32);
    CHECK(compare_agg_vs_raw(&win, &rp_ho.res, nodes, nn, 1) == 0,
          "half-open raw predicate must agree bit-exactly");

    raw_pairs_free(&rp_incl);
    raw_pairs_free(&rp_ho);
    pgwt_block_agg_free(&win);
    for (int i = 0; i < nb; i++)
        pgwt_block_agg_free(&per_block[i]);
}

/* ── §7 mutation probes ─────────────────────────────────────────────────── */

/* The comparator above is the only thing standing between a wrong number and
 * a green suite. "0 == 0" satisfies every assertion in this file, so the
 * comparator is proven able to SEE each kind of error before any agreement is
 * claimed. Each probe perturbs the aggregate by the smallest possible amount
 * and requires a disagreement. */
static void section7_mutation_probes(void)
{
    printf("=== §7 mutation probes: the comparator can see one count, one "
           "nanosecond, one pair ===\n");

    struct fix_block b[FIX_MAX_BLOCKS];
    int nb = build_model_fixture(b, FIX_MAX_BLOCKS);
    struct pgwt_block_agg per_block[FIX_MAX_BLOCKS];
    int have_agg[FIX_MAX_BLOCKS];
    for (int i = 0; i < nb; i++)
        have_agg[i] = pgwt_block_agg_build(&per_block[i], &b[i].id, b[i].type,
                                           b[i].committed, b[i].ev, b[i].n)
                      == PGWT_BAGG_OK;

    uint64_t from = 0, to = UINT64_MAX;
    struct pgwt_trace_event raw[FIX_MAX_EV * FIX_MAX_BLOCKS];
    int n = collect_window(b, nb, from, to, 0, raw,
                           (int)(sizeof(raw) / sizeof(raw[0])));
    struct raw_pairs rp;
    raw_pairs_of(&rp, raw, n);
    struct node_oracle_row nodes[32];
    int nn = raw_node_oracle(raw, n, nodes, 32);

    const char *labels[] = {
        "clean (control)", "+1 on one pair count", "+1 ns on one pair",
        "-1 ns on one pair", "an extra pair appears",
        "one pair disappears", "+1 ns on one node", "one node disappears",
    };
    for (int probe = 0; probe < 8; probe++) {
        struct pgwt_block_agg win;
        int nm, nd, ns;
        CHECK(answer_window_phase1(b, nb, per_block, have_agg, from, to, &win,
                                   &nm, &nd, &ns) == PGWT_BAGG_OK,
              "probe %d answer", probe);
        /* Find the first occupied slot in each table to perturb. */
        int ps = -1, nsl = -1;
        for (int i = 0; i < win.pair_cap; i++)
            if (win.pairs[i].count) { ps = i; break; }
        for (int i = 0; i < win.node_cap; i++)
            if (win.nodes[i].count) { nsl = i; break; }
        CHECK(ps >= 0 && nsl >= 0, "probe %d found nothing to perturb — the "
              "aggregate is empty, which would make every probe vacuous",
              probe);

        switch (probe) {
        case 0: break;                                   /* control */
        case 1: win.pairs[ps].count += 1; win.total_transitions += 1; break;
        case 2: win.pairs[ps].total_ns += 1; win.pair_total_ns += 1; break;
        case 3: win.pairs[ps].total_ns -= 1; win.pair_total_ns -= 1; break;
        case 4: pgwt_block_agg_add_event(&win, &(struct pgwt_trace_event){
                    .timestamp_ns = 1, .old_event = E_LOCK1,
                    .new_event = E_LW1, .duration_ns = 1 }); break;
        case 5: win.pairs[ps].count = 0; win.n_pairs--; break;
        case 6: win.nodes[nsl].total_ns += 1; win.node_total_ns += 1; break;
        case 7: win.nodes[nsl].count = 0; win.n_nodes--; break;
        }

        int bad = compare_agg_vs_raw(&win, &rp.res, nodes, nn, 0);
        if (probe == 0) {
            CHECK(bad == 0, "control probe must be clean, got %d", bad);
        } else {
            CHECK(bad > 0, "probe '%s' went UNDETECTED — the comparator is "
                  "blind to it", labels[probe]);
            if (bad > 0)
                seen.mutation_probes_went_red++;
        }
        printf("    probe %d (%-24s) disagreements=%d\n", probe, labels[probe],
               bad);
        pgwt_block_agg_free(&win);
    }
    raw_pairs_free(&rp);
    for (int i = 0; i < nb; i++)
        pgwt_block_agg_free(&per_block[i]);
}

/* ── §8 bypass suite ────────────────────────────────────────────────────── */

/* Every way THIS gate's detection can be made unreachable. Showing the gate
 * can go red (§7) proves the true-positive path, which has never been the
 * broken one; what follows is the false-negative side — the cases where the
 * fast path could approve without having looked at anything. Each one must
 * REFUSE, not approve. */
static void section8_bypass(void)
{
    printf("=== §8 bypass suite: a path that cannot see must refuse ===\n");

    struct fix_block b;
    memset(&b, 0, sizeof(b));
    ev_set(&b, 1000, 11, E_CPU, E_IO1, 100, 0);
    ev_set(&b, 1100, 11, E_IO1, E_CPU, 200, 0);
    fix_seal(&b, &TRACE_A, 7, 7168);

    struct pgwt_block_agg a;

    /* B1 EMPTY block: a SUCCESS with zero pairs. "This block contributes
     * nothing" is a fact and must be distinguishable from a refusal — the
     * distinction is the return code, not the contents. */
    struct fix_block empty;
    memset(&empty, 0, sizeof(empty));
    ev_set(&empty, 9000, 11, PGWT_MARKER_EXEC_START, PGWT_MARKER_EXEC_START,
           0, 0);
    fix_seal(&empty, &TRACE_A, 8, 8192);
    CHECK(pgwt_block_agg_build(&a, &empty.id, empty.type, 1, empty.ev, empty.n)
          == PGWT_BAGG_OK, "B1 a block with no qualifying record must BUILD");
    CHECK(a.n_pairs == 0 && a.total_transitions == 0,
          "B1 empty block must aggregate to zero");
    CHECK(a.mode == PGWT_BAGG_MODE_BLOCK,
          "B1 an empty aggregate is still a valid aggregate");
    {   /* merging it is a no-op on the numbers, not a refusal */
        struct pgwt_block_agg w;
        pgwt_block_agg_init_window(&w);
        CHECK(pgwt_block_agg_merge(&w, &a) == PGWT_BAGG_OK, "B1 merge empty");
        CHECK(w.total_transitions == 0 && w.n_keys == 1,
              "B1 merging an empty block must still record the block");
        pgwt_block_agg_free(&w);
    }
    pgwt_block_agg_free(&a);

    /* B2 NO aggregate at all: plan must DECODE where it would have MERGEd. */
    CHECK(pgwt_block_agg_plan(&b.id, 1, 900, 1200) == PGWT_BLOCK_MERGE,
          "B2 precondition: this block would merge");
    CHECK(pgwt_block_agg_plan(&b.id, 0, 900, 1200) == PGWT_BLOCK_DECODE,
          "B2 absent aggregate must DECODE, never MERGE");
    seen.refusals_observed++;

    /* B3 a SAMPLES block (C1). */
    CHECK(pgwt_block_agg_build(&a, &b.id, PGWT_BLOCK_SAMPLES, 1, b.ev, b.n)
          == PGWT_BAGG_REFUSED_BLOCK_TYPE, "B3 SAMPLES must be refused");
    CHECK(a.mode == PGWT_BAGG_MODE_NONE,
          "B3 a refused build must not leave a usable aggregate");
    seen.refusals_observed++;

    /* B4 an uncommitted block (C2) — the open block of a current.trace. */
    CHECK(pgwt_block_agg_build(&a, &b.id, b.type, 0, b.ev, b.n)
          == PGWT_BAGG_REFUSED_UNCOMMITTED, "B4 uncommitted must be refused");
    CHECK(a.mode == PGWT_BAGG_MODE_NONE, "B4 no usable aggregate");
    seen.refusals_observed++;

    /* B5 an UNRESOLVABLE identity (the header could not be read): unknown is
     * never a match, and must never build. */
    struct pgwt_block_identity blank = b.id;
    memset(&blank.trace, 0, sizeof(blank.trace));
    CHECK(pgwt_trace_identity_resolvable(&blank.trace) == 0,
          "B5 an all-zero trace identity is not resolvable");
    CHECK(pgwt_block_agg_build(&a, &blank, PGWT_BLOCK_TRANSITIONS, 1,
                               b.ev, b.n) == PGWT_BAGG_REFUSED_INVALID,
          "B5 unresolvable identity must be refused");
    seen.refusals_observed++;
    {   /* and two unresolvable identities must not compare equal */
        struct pgwt_block_agg x;
        CHECK(pgwt_block_agg_build(&x, &b.id, b.type, 1, b.ev, b.n)
              == PGWT_BAGG_OK, "B5 control build");
        CHECK(pgwt_block_agg_matches(&x, &blank) == 0,
              "B5 matches() must refuse an unresolvable identity");
        struct pgwt_block_agg y = x;
        memset(&y.id.trace, 0, sizeof(y.id.trace));
        CHECK(pgwt_block_agg_matches(&y, &blank) == 0,
              "B5 zero vs zero must NOT match");
        pgwt_block_agg_free(&x);
        seen.refusals_observed++;
    }

    /* B6 VERSION mismatch (#315: a version whose contract is not true must
     * not be read). */
    {
        struct pgwt_block_agg src, w;
        CHECK(pgwt_block_agg_build(&src, &b.id, b.type, 1, b.ev, b.n)
              == PGWT_BAGG_OK, "B6 build");
        src.version = PGWT_BLOCK_AGG_VERSION + 1;
        pgwt_block_agg_init_window(&w);
        CHECK(pgwt_block_agg_merge(&w, &src) == PGWT_BAGG_REFUSED_VERSION,
              "B6 a foreign version must be refused");
        CHECK(w.total_transitions == 0, "B6 refusal must not move numbers");
        CHECK(pgwt_block_agg_matches(&src, &b.id) == 0,
              "B6 matches() must refuse a foreign version");
        /* and the other direction: a stale accumulator */
        src.version = PGWT_BLOCK_AGG_VERSION;
        w.version = PGWT_BLOCK_AGG_VERSION + 1;
        CHECK(pgwt_block_agg_merge(&w, &src) == PGWT_BAGG_REFUSED_VERSION,
              "B6 a foreign destination version must be refused");
        CHECK(pgwt_block_agg_add_event(&w, &b.ev[0])
              == PGWT_BAGG_REFUSED_VERSION,
              "B6 add_event into a foreign-version accumulator must refuse");
        w.version = PGWT_BLOCK_AGG_VERSION;
        pgwt_block_agg_free(&w);
        pgwt_block_agg_free(&src);
        seen.refusals_observed += 4;
    }

    /* B7 ROTATION: blocks from two different trace files. Their keys differ,
     * so both must merge and SUM; but a trace-pinned comparison must refuse. */
    {
        struct fix_block rb = b;
        fix_seal(&rb, &TRACE_B, 7, 7168);   /* same index + offset, new file */
        struct pgwt_block_agg aa, ab, w;
        CHECK(pgwt_block_agg_build(&aa, &b.id, b.type, 1, b.ev, b.n)
              == PGWT_BAGG_OK, "B7 build A");
        CHECK(pgwt_block_agg_build(&ab, &rb.id, rb.type, 1, rb.ev, rb.n)
              == PGWT_BAGG_OK, "B7 build B");
        CHECK(pgwt_block_agg_matches(&aa, &rb.id) == 0,
              "B7 an aggregate from the pre-rotation file must NOT match the "
              "post-rotation block at the same index and offset");
        seen.refusals_observed++;
        pgwt_block_agg_init_window(&w);
        CHECK(pgwt_block_agg_merge(&w, &aa) == PGWT_BAGG_OK, "B7 merge A");
        CHECK(pgwt_block_agg_merge(&w, &ab) == PGWT_BAGG_OK,
              "B7 the same index in a DIFFERENT file is not a duplicate");
        CHECK(w.total_transitions == 2 * aa.total_transitions,
              "B7 both files' blocks must sum");
        pgwt_block_agg_free(&w);
        pgwt_block_agg_free(&aa);
        pgwt_block_agg_free(&ab);
    }

    /* B8 a REWRITTEN block: every header-derived field, perturbed one at a
     * time, must break matches(). A loop, so adding a field without adding it
     * to matches() shows up as an unperturbed field here. */
    {
        struct pgwt_block_agg x;
        CHECK(pgwt_block_agg_build(&x, &b.id, b.type, 1, b.ev, b.n)
              == PGWT_BAGG_OK, "B8 build");
        CHECK(pgwt_block_agg_matches(&x, &b.id) == 1,
              "B8 control: the unmodified identity must match");
        struct { const char *name; size_t off; size_t sz; } fields[] = {
            { "trace.trace_version",
              offsetof(struct pgwt_block_identity, trace.trace_version), 4 },
            { "trace.pg_version",
              offsetof(struct pgwt_block_identity, trace.pg_version), 4 },
            { "trace.start_time_ns",
              offsetof(struct pgwt_block_identity, trace.start_time_ns), 8 },
            { "trace.clock_offset_ns",
              offsetof(struct pgwt_block_identity, trace.clock_offset_ns), 8 },
            { "block_index",
              offsetof(struct pgwt_block_identity, block_index), 4 },
            { "num_events",
              offsetof(struct pgwt_block_identity, num_events), 4 },
            { "file_offset",
              offsetof(struct pgwt_block_identity, file_offset), 8 },
            { "first_timestamp_ns",
              offsetof(struct pgwt_block_identity, first_timestamp_ns), 8 },
            { "last_timestamp_ns",
              offsetof(struct pgwt_block_identity, last_timestamp_ns), 8 },
        };
        for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
            struct pgwt_block_identity now = b.id;
            unsigned char *p = (unsigned char *)&now + fields[f].off;
            for (size_t k = 0; k < fields[f].sz; k++)
                p[k] ^= (k == 0 ? 1 : 0);   /* +/-1 in the low byte */
            CHECK(pgwt_block_agg_matches(&x, &now) == 0,
                  "B8 a block whose %s changed must NOT match", fields[f].name);
            seen.refusals_observed++;
        }
        /* The payload check, for a caller that decoded anyway. */
        struct pgwt_trace_event tampered[FIX_MAX_EV];
        memcpy(tampered, b.ev, sizeof(b.ev[0]) * (size_t)b.n);
        tampered[0].duration_ns += 1;
        CHECK(pgwt_block_agg_verify_payload(&x, b.ev, b.n) == 1,
              "B8 payload control");
        CHECK(pgwt_block_agg_verify_payload(&x, tampered, b.n) == 0,
              "B8 one nanosecond of tampered payload must fail verification");
        CHECK(pgwt_block_agg_verify_payload(&x, b.ev, b.n - 1) == 0,
              "B8 a truncated payload must fail verification");
        seen.refusals_observed += 2;
        pgwt_block_agg_free(&x);
    }

    /* B9 DOUBLE MERGE of the same block — the double-count guard. */
    {
        struct pgwt_block_agg x, w;
        CHECK(pgwt_block_agg_build(&x, &b.id, b.type, 1, b.ev, b.n)
              == PGWT_BAGG_OK, "B9 build");
        pgwt_block_agg_init_window(&w);
        CHECK(pgwt_block_agg_merge(&w, &x) == PGWT_BAGG_OK, "B9 first merge");
        uint64_t t0 = w.total_transitions, ns0 = w.pair_total_ns;
        int np0 = w.n_pairs;
        CHECK(pgwt_block_agg_merge(&w, &x) == PGWT_BAGG_REFUSED_DUPLICATE,
              "B9 merging the same block twice must be REFUSED");
        CHECK(w.total_transitions == t0 && w.pair_total_ns == ns0 &&
              w.n_pairs == np0,
              "B9 the refused duplicate must leave the accumulator untouched");
        seen.refusals_observed++;
        /* A distinct copy of the same block (same identity) is the same block. */
        struct pgwt_block_agg y;
        CHECK(pgwt_block_agg_build(&y, &b.id, b.type, 1, b.ev, b.n)
              == PGWT_BAGG_OK, "B9 rebuild");
        CHECK(pgwt_block_agg_merge(&w, &y) == PGWT_BAGG_REFUSED_DUPLICATE,
              "B9 a re-read of the same block is still a duplicate");
        seen.refusals_observed++;
        pgwt_block_agg_free(&y);
        pgwt_block_agg_free(&x);
        pgwt_block_agg_free(&w);
    }

    /* B10 ALLOCATION FAILURE: refuse whole, never a short aggregate. This is
     * the C analogue of "a dependency exited 126/127" — the thing the path
     * needs is unavailable, so the path must decline, not answer. */
    {
        const char *points[] = { "block_agg_pairs_grow", "block_agg_nodes_grow" };
        for (size_t p = 0; p < sizeof(points) / sizeof(points[0]); p++) {
            setenv("PGWT_TEST_ALLOC_FAIL", points[p], 1);
            struct pgwt_block_agg x;
            int rc = pgwt_block_agg_build(&x, &b.id, b.type, 1, b.ev, b.n);
            unsetenv("PGWT_TEST_ALLOC_FAIL");
            CHECK(rc == PGWT_BAGG_REFUSED_NOMEM,
                  "B10 %s must refuse with NOMEM, got %d", points[p], rc);
            CHECK(x.mode == PGWT_BAGG_MODE_NONE && x.total_transitions == 0,
                  "B10 %s must leave NO usable aggregate — a short one would "
                  "be a silently wrong answer", points[p]);
            seen.refusals_observed++;
        }
        /* the key-set growth, on the merge side */
        setenv("PGWT_TEST_ALLOC_FAIL", "block_agg_keys_grow", 1);
        struct pgwt_block_agg x, w;
        int rc = pgwt_block_agg_build(&x, &b.id, b.type, 1, b.ev, b.n);
        pgwt_block_agg_init_window(&w);
        int mrc = (rc == PGWT_BAGG_OK) ? pgwt_block_agg_merge(&w, &x) : rc;
        unsetenv("PGWT_TEST_ALLOC_FAIL");
        CHECK(mrc == PGWT_BAGG_REFUSED_NOMEM,
              "B10 block_agg_keys_grow must refuse the merge, got %d", mrc);
        CHECK(w.total_transitions == 0,
              "B10 a merge refused for memory must not have moved numbers");
        seen.refusals_observed++;
        pgwt_block_agg_free(&w);
        if (rc == PGWT_BAGG_OK) pgwt_block_agg_free(&x);

        /* The pair table failing to grow DURING a merge. There is no
         * rollback, so the accumulator can be left partial — the contract
         * (block_agg.h) is that the caller discards it. What must never
         * happen is a PGWT_BAGG_OK return on a merge that did not complete. */
        struct pgwt_block_agg mx, mw;
        CHECK(pgwt_block_agg_build(&mx, &b.id, b.type, 1, b.ev, b.n)
              == PGWT_BAGG_OK, "B10 merge-NOMEM build");
        pgwt_block_agg_init_window(&mw);
        setenv("PGWT_TEST_ALLOC_FAIL", "block_agg_pairs_grow", 1);
        int mrc2 = pgwt_block_agg_merge(&mw, &mx);
        unsetenv("PGWT_TEST_ALLOC_FAIL");
        CHECK(mrc2 == PGWT_BAGG_REFUSED_NOMEM,
              "B10 a merge whose pair table cannot grow must return NOMEM, "
              "never OK — got %d", mrc2);
        seen.refusals_observed++;
        pgwt_block_agg_free(&mw);
        pgwt_block_agg_free(&mx);
        /* and the readout */
        struct pgwt_block_agg z;
        CHECK(pgwt_block_agg_build(&z, &b.id, b.type, 1, b.ev, b.n)
              == PGWT_BAGG_OK, "B10 readout build");
        setenv("PGWT_TEST_ALLOC_FAIL", "block_agg_pairs_sorted", 1);
        struct pgwt_block_agg_pair *out = (void *)0x1;
        int nout = -1;
        rc = pgwt_block_agg_pairs_sorted(&z, &out, &nout);
        unsetenv("PGWT_TEST_ALLOC_FAIL");
        CHECK(rc == PGWT_BAGG_REFUSED_NOMEM,
              "B10 pairs_sorted must refuse with NOMEM, got %d", rc);
        CHECK(out == NULL && nout == 0,
              "B10 a refused readout must not hand back a stale pointer or a "
              "plausible zero count");
        seen.refusals_observed++;
        pgwt_block_agg_free(&z);
    }

    /* B11 FILTERS (C3): the aggregate keeps no filter dimension, so every
     * filtered request must fall back to raw. One case per field, because a
     * single "is the struct zero" test would silently accept a new field. */
    {
        struct pgwt_filter f;
        memset(&f, 0, sizeof(f));
        CHECK(pgwt_block_agg_filter_supported(NULL) == 1, "B11 NULL = no filter");
        CHECK(pgwt_block_agg_filter_supported(&f) == 1, "B11 empty filter");
        memset(&f, 0, sizeof(f)); f.pid = 42;
        CHECK(pgwt_block_agg_filter_supported(&f) == 0, "B11 pid filter");
        memset(&f, 0, sizeof(f)); f.event_id = 7;
        CHECK(pgwt_block_agg_filter_supported(&f) == 0, "B11 event filter");
        memset(&f, 0, sizeof(f)); f.query_id = 99;
        CHECK(pgwt_block_agg_filter_supported(&f) == 0, "B11 query filter");
        memset(&f, 0, sizeof(f)); snprintf(f.class_name, sizeof(f.class_name),
                                           "IO");
        CHECK(pgwt_block_agg_filter_supported(&f) == 0, "B11 class filter");
        seen.refusals_observed += 4;
    }

    /* B12 a window that selects nothing, and a nonsense block header. */
    CHECK(pgwt_block_agg_plan(&b.id, 1, 1000, 1000) == PGWT_BLOCK_SKIP,
          "B12 an empty window selects nothing");
    CHECK(pgwt_block_agg_plan(&b.id, 1, 2000, 1000) == PGWT_BLOCK_SKIP,
          "B12 an inverted window selects nothing");
    CHECK(pgwt_block_agg_plan(NULL, 1, 0, 1000) == PGWT_BLOCK_SKIP,
          "B12 no identity selects nothing");
    {
        struct pgwt_block_identity bad = b.id;
        bad.last_timestamp_ns = bad.first_timestamp_ns - 1;   /* unparseable */
        CHECK(pgwt_block_agg_plan(&bad, 1, 0, UINT64_MAX) == PGWT_BLOCK_DECODE,
              "B12 a block with nonsense bounds must DECODE, never MERGE");
        struct pgwt_block_agg x;
        CHECK(pgwt_block_agg_build(&x, &bad, PGWT_BLOCK_TRANSITIONS, 1,
                                   b.ev, b.n) == PGWT_BAGG_REFUSED_INVALID,
              "B12 a block with nonsense bounds must not build");
        seen.refusals_observed += 2;
    }

    /* B13 MODE: a built block aggregate is immutable, and a window is not a
     * block. Both directions refuse, so no caller can quietly turn an
     * accumulator into something the cache will later revalidate. */
    {
        struct pgwt_block_agg x, w, none;
        memset(&none, 0, sizeof(none));
        CHECK(pgwt_block_agg_build(&x, &b.id, b.type, 1, b.ev, b.n)
              == PGWT_BAGG_OK, "B13 build");
        CHECK(pgwt_block_agg_add_event(&x, &b.ev[0]) == PGWT_BAGG_REFUSED_MODE,
              "B13 a built block aggregate must be immutable");
        pgwt_block_agg_init_window(&w);
        CHECK(pgwt_block_agg_merge(&x, &x) == PGWT_BAGG_REFUSED_MODE,
              "B13 merging into a block aggregate must refuse");
        CHECK(pgwt_block_agg_merge(&w, &w) == PGWT_BAGG_REFUSED_MODE,
              "B13 merging a window into a window must refuse");
        CHECK(pgwt_block_agg_merge(&none, &x) == PGWT_BAGG_REFUSED_MODE,
              "B13 merging into an uninitialised aggregate must refuse");
        CHECK(pgwt_block_agg_add_event(&none, &b.ev[0])
              == PGWT_BAGG_REFUSED_VERSION ||
              pgwt_block_agg_add_event(&none, &b.ev[0])
              == PGWT_BAGG_REFUSED_MODE,
              "B13 add_event into an uninitialised aggregate must refuse");
        CHECK(pgwt_block_agg_lookup(&none, E_CPU, E_IO1, NULL, NULL) == 0,
              "B13 an uninitialised aggregate answers nothing");
        CHECK(pgwt_block_agg_matches(&w, &b.id) == 0,
              "B13 a window accumulator is not a block and must not match one");
        seen.refusals_observed += 6;
        pgwt_block_agg_free(&w);
        pgwt_block_agg_free(&x);
    }

    /* B14 a freed aggregate must answer nothing (use-after-free of the
     * DECISION, not of memory: a stale struct must not look usable). */
    {
        struct pgwt_block_agg x;
        CHECK(pgwt_block_agg_build(&x, &b.id, b.type, 1, b.ev, b.n)
              == PGWT_BAGG_OK, "B14 build");
        pgwt_block_agg_free(&x);
        CHECK(x.mode == PGWT_BAGG_MODE_NONE, "B14 freed => MODE_NONE");
        CHECK(pgwt_block_agg_lookup(&x, E_CPU, E_IO1, NULL, NULL) == 0,
              "B14 a freed aggregate must answer nothing");
        CHECK(pgwt_block_agg_matches(&x, &b.id) == 0,
              "B14 a freed aggregate must not match");
        struct pgwt_block_agg_pair *o = (void *)0x1;
        int n = -1;
        CHECK(pgwt_block_agg_pairs_sorted(&x, &o, &n)
              == PGWT_BAGG_REFUSED_INVALID,
              "B14 readout from a freed aggregate must refuse");
        seen.refusals_observed += 3;
    }

    /* B15 NOTHING QUALIFIES: a real window over records that all fail the
     * predicate. Both sides are empty and every assertion in this file is
     * satisfied by 0 == 0, which is exactly why this case is checked
     * SEPARATELY and is NOT counted toward the vacuity budget. */
    {
        struct fix_block nq;
        memset(&nq, 0, sizeof(nq));
        ev_set(&nq, 7000, 11, E_HIDE, E_HIDE, 500, 0);
        ev_set(&nq, 7100, 11, PGWT_MARKER_EXEC_START, E_IO1, 0, 0);
        ev_set(&nq, 7200, 11, E_CPU, E_IO1, 600, PGWT_EVENT_FLAG_SAMPLE);
        fix_seal(&nq, &TRACE_A, 9, 9216);
        struct pgwt_block_agg x, w;
        int ha = 1;
        CHECK(pgwt_block_agg_build(&x, &nq.id, nq.type, 1, nq.ev, nq.n)
              == PGWT_BAGG_OK, "B15 build");
        CHECK(x.n_pairs == 0 && x.n_nodes == 0,
              "B15 nothing must qualify in this fixture (n_pairs=%d n_nodes=%d)",
              x.n_pairs, x.n_nodes);
        int nm, nd, ns;
        CHECK(answer_window_phase1(&nq, 1, &x, &ha, 0, UINT64_MAX, &w,
                                   &nm, &nd, &ns) == PGWT_BAGG_OK, "B15 answer");
        struct pgwt_trace_event raw[FIX_MAX_EV];
        int n = collect_window(&nq, 1, 0, UINT64_MAX, 0, raw, FIX_MAX_EV);
        struct raw_pairs rp;
        raw_pairs_of(&rp, raw, n);
        struct node_oracle_row nodes[8];
        int nn = raw_node_oracle(raw, n, nodes, 8);
        CHECK(compare_agg_vs_raw(&w, &rp.res, nodes, nn, 1) == 0,
              "B15 both sides empty must still AGREE");
        CHECK(rp.res.total_transitions == 0 && nn == 0,
              "B15 precondition: the raw path must also find nothing");
        raw_pairs_free(&rp);
        pgwt_block_agg_free(&w);
        pgwt_block_agg_free(&x);
    }
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("test_block_agg — per-committed-block transition aggregate "
           "(Phase 1)\n");

    section1_predicate_differential();
    section2_literals();
    section3_merge_algebra();
    section4_straddle();
    section5_plan_partition();
    section6_model();
    section6b_inclusive_end_divergence();
    section7_mutation_probes();
    section8_bypass();

    /* §0 the ledger. A green run that exercised nothing is not a pass. */
    printf("=== §0 vacuity ledger ===\n");
    printf("    windows compared .................. %ld\n",
           seen.windows_compared);
    printf("    blocks MERGEd ..................... %ld\n", seen.plan_merge);
    printf("    blocks DECODEd .................... %ld\n", seen.plan_decode);
    printf("    blocks SKIPped .................... %ld\n", seen.plan_skip);
    printf("    windows at a merge/decode seam .... %ld\n",
           seen.windows_with_merge_and_decode);
    printf("    windows with positive total_ns .... %ld\n",
           seen.windows_with_positive_total_ns);
    printf("    windows with >=1 merged pair ...... %ld\n",
           seen.windows_with_nonzero_pairs);
    printf("    mutation probes that went red ..... %ld\n",
           seen.mutation_probes_went_red);
    printf("    refusals observed ................. %ld\n",
           seen.refusals_observed);

    CHECK(seen.windows_compared > 0, "no window was compared at all");
    CHECK(seen.plan_merge > 0, "no block was ever MERGEd — the fast path was "
          "never exercised");
    CHECK(seen.plan_decode > 0, "no block was ever DECODEd — the boundary path "
          "was never exercised");
    CHECK(seen.plan_skip > 0, "no block was ever SKIPped");
    CHECK(seen.windows_with_merge_and_decode > 0,
          "no window had a merged block AND a decoded block at once — the "
          "SEAM, where double counting lives, was never tested");
    CHECK(seen.windows_with_positive_total_ns > 0,
          "every compared total_ns was zero — 0 == 0 proves nothing");
    CHECK(seen.windows_with_nonzero_pairs > 0,
          "every merged aggregate was empty");
    CHECK(seen.mutation_probes_went_red == 7,
          "expected 7 mutation probes to go red, got %ld",
          seen.mutation_probes_went_red);
    CHECK(seen.refusals_observed > 25,
          "only %ld refusals observed — the bypass suite did not run",
          seen.refusals_observed);

    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS: test_block_agg\n");
    return 0;
}
