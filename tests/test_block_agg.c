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
 *   §1b C7 at FILE granularity: the per-file window gate, with a boundary
 *       sweep against the raw loader's rule transcribed independently
 *   §2 build + merge against literal expected values
 *   §3 merge is associative and commutative
 *   §4 the straddle rule (a pair whose old_event began in an earlier block)
 *   §5 plan() is a partition: exactly one of SKIP/MERGE/DECODE per block
 *   §6 the model test: merged-plus-decoded == raw, over every window
 *   §6b src/server.c's inclusive end bound, pinned in BOTH directions with
 *       the same 163 ns literal: zero divergence from the loader, and
 *       exactly one record lost under a half-open end bound. (This was a
 *       "known divergence" until c27e084 reconciled the aggregate TO the
 *       loader; it is now an agreement, and the literal is what holds it.)
 *   §7 mutation probes: the comparator can see one count / one ns / one pair
 *   §8 bypass suite: every way this gate's detection can be made unreachable
 *   §9 on-disk, through the real writer/reader, driving the very function
 *      src/server.c's handle_transitions() calls — twice, so the cache's
 *      "a merged aggregate equals a fresh decode" contract is exercised
 */
#include "block_agg.h"
#include "compute.h"
#include "idle_rule.h"
#include "summary_reader.h"
#include "wait_event.h"      /* pgwt_class_name, for §10b's class filter */
#include "event_reader.h"
#include "event_writer.h"

#include <assert.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
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
    /* Phase 2 (§10-§12). Each one FAILS the run at zero: the per-event path
     * has three ways to look exercised while being untested — a fixture with
     * no straddling wait (no clip), a fixture where every event is in its own
     * class (the class fold is a relabelling), and an oracle that returned no
     * rows (the row loop never ran). */
    long ev_predicate_cases;
    long ev_windows_compared;
    long ev_windows_with_rows;
    long ev_windows_at_seam;
    long event_rows_compared;
    long ev_filtered_rows_compared;
    long class_rows_compared;
    long class_rows_multi;          /* classes holding >= 2 distinct events */
    long ev_clipped_records;        /* admitted records whose clip < duration */
    long ev_merge_refused_clip;     /* REFUSED_CLIP -> DECODE downgrades */
    long ev_shipped_rows_compared;
    long ev_shipped_clip_downgrades;
    long ev_filter_unselectable;    /* event_id 0: not selectable by filter */
    long ev_bucket_pairs_checked;
    long v2_probes_went_red;
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
 * a disagreement here rather than a silent agreement.
 *
 * The marker range below is spelled out as literals for that independence,
 * which creates a second hazard: if PGWT_IS_MARKER's range ever moves, this
 * oracle keeps testing the OLD range and agrees with nothing in particular.
 * Independence from the predicate must not mean independence from reality, so
 * the literals are pinned to the real macro at compile time. This fails the
 * BUILD rather than a test, deliberately — a drifted oracle is not a test
 * result, it is an instrument that stopped measuring. */
_Static_assert(PGWT_IS_MARKER(0xFFFFFFF0U) && PGWT_IS_MARKER(0xFFFFFFF7U) &&
               !PGWT_IS_MARKER(0xFFFFFFEFU) && !PGWT_IS_MARKER(0xFFFFFFF8U),
               "raw_node_oracle's hardcoded marker range [0xFFFFFFF0, "
               "0xFFFFFFF7] no longer matches PGWT_IS_MARKER — update the "
               "oracle's literals, do not relax this assertion");
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

    /* C7 at the two bounds and one past each. INCLUSIVE at both ends, not
     * half-open -- see the ts == to assertion three lines down. The stale
     * "half-open" wording that used to head this block is the exact
     * misreading that cost this branch a wiring round and took
     * test_data_aas's Total AAS from 4.0 to 0. */
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = 500;
    CHECK(pgwt_block_agg_in_window(&e, 500, 600) == 1, "ts == from is in");
    CHECK(pgwt_block_agg_in_window(&e, 501, 600) == 0, "ts < from is out");
    e.timestamp_ns = 600;
    CHECK(pgwt_block_agg_in_window(&e, 500, 600) == 1,
          "ts == to is IN: timestamp_ns is when the wait ENDED, so an event "
          "ending exactly at `to` lies wholly inside the window (C7)");
    e.timestamp_ns = 601;
    CHECK(pgwt_block_agg_in_window(&e, 500, 600) == 0, "ts == to+1 is out");
    e.timestamp_ns = 599;
    CHECK(pgwt_block_agg_in_window(&e, 500, 600) == 1, "ts == to-1 is in");

    /* §1b C7 at FILE granularity — the same rule one layer up, where it
     * drifted and shipped. transitions_from_block_aggs() used
     * `mono_first >= to_m` while the raw loader (src/server.c:2689) and the
     * marker loader (:3089) both use `>`, so a file whose EARLIEST record
     * ends exactly at `to_m` was skipped WHOLE by the fast path and admitted
     * by raw: `total` and one link's value short by exactly those records,
     * with no refusal and no fidelity change.
     *
     * This is mutation M21's off-by-one at a granularity M21 never reached.
     * §5/§6/§9 pin it per BLOCK; nothing pinned it per FILE, so the identical
     * error got through the suite built to catch it. The literals below are
     * the whole point: 600 == to must CONTRIBUTE, 601 must not.
     *
     * Honest limit: this pins the shared PREDICATE that the shipping site now
     * calls, not transitions_from_block_aggs()' multi-file loop itself (that
     * function is static and takes a live struct pgwt_server). Mutation M22 in
     * run_mutations.py flips the comparison back at the shipping line and must
     * go RED, which is what ties this assertion to the code that ships. */
    CHECK(pgwt_block_agg_file_can_contribute(600, 900, 500, 600) == 1,
          "§1b a file whose FIRST record ends exactly at `to` CONTRIBUTES — "
          "`mono_first >= to_m` was the shipped bug");
    CHECK(pgwt_block_agg_file_can_contribute(601, 900, 500, 600) == 0,
          "§1b mono_first == to+1 cannot contribute");
    CHECK(pgwt_block_agg_file_can_contribute(100, 500, 500, 600) == 1,
          "§1b a file whose LAST record ends exactly at `from` CONTRIBUTES — "
          "the mirror image, which is why `last < from` is not `<=`");
    CHECK(pgwt_block_agg_file_can_contribute(100, 499, 500, 600) == 0,
          "§1b mono_last == from-1 cannot contribute");
    CHECK(pgwt_block_agg_file_can_contribute(500, 600, 500, 600) == 1,
          "§1b a file exactly spanning the window contributes");
    CHECK(pgwt_block_agg_file_can_contribute(600, 600, 600, 600) == 1,
          "§1b a degenerate single-instant file at a single-instant window "
          "contributes: both bounds inclusive, so this is not empty");

    /* The predicate must agree with the raw loader's convention on the very
     * instants where they could differ. Asserted as a property over a swept
     * range rather than the two literals above, so a future edit that fixes
     * one bound and breaks the other cannot pass. */
    {
        int disagreements = 0, exercised = 0;
        for (uint64_t first = 498; first <= 602; first++) {
            for (uint64_t last = first; last <= 602; last++) {
                /* The raw/marker loaders' rule, written out independently
                 * rather than by calling the predicate under test. */
                int raw_admits = !(first > 600 || last < 500);
                int agg_admits =
                    pgwt_block_agg_file_can_contribute(first, last, 500, 600);
                exercised++;
                if (raw_admits != agg_admits)
                    disagreements++;
            }
        }
        CHECK(exercised > 1000, "§1b the sweep must actually run (%d cases)",
              exercised);
        CHECK(disagreements == 0,
              "§1b the file predicate must agree with the raw loader's "
              "`first > to || last < from` rule on every boundary instant "
              "(%d/%d disagreed)", disagreements, exercised);
    }
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
    struct pgwt_block_agg_pair *sp2 = sp;   /* kept for §3c's row comparison */
    int spn2 = spn;

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

    /* §3c the RAW path's row order must be the SAME total order.
     *
     * pgwt_compute_transitions()' comparator used to order by count alone,
     * which is not a total order, so equal-count links came out in whatever
     * sequence qsort made of the hash table's layout. Two consequences: the
     * Transitions tab could reorder tied rows between identical requests, and
     * a row-by-row comparison against the aggregate's readout was meaningless.
     * Both sides now break ties by (from ASC, to ASC). Asserted here against
     * the same tie-heavy fixture, because the set-based comparator used
     * everywhere else in this file cannot see row ORDER at all. */
    struct pgwt_filter nof;
    memset(&nof, 0, sizeof(nof));
    struct pgwt_transitions_result rawr;
    pgwt_compute_transitions(tie.ev, tie.n, &nof, ORACLE_MAX_ROWS, &rawr);
    CHECK(rawr.num_rows == spn,
          "§3c raw found %d rows, the aggregate %d", rawr.num_rows, spn);
    int order_ties = 0;
    for (int i = 0; i + 1 < rawr.num_rows; i++) {
        if (rawr.rows[i].count == rawr.rows[i + 1].count)
            order_ties++;
        CHECK(rawr.rows[i].count > rawr.rows[i + 1].count ||
              (rawr.rows[i].count == rawr.rows[i + 1].count &&
               (rawr.rows[i].from_event < rawr.rows[i + 1].from_event ||
                (rawr.rows[i].from_event == rawr.rows[i + 1].from_event &&
                 rawr.rows[i].to_event < rawr.rows[i + 1].to_event))),
              "§3c raw rows %d/%d violate (count desc, from asc, to asc): "
              "(%u->%u, %" PRIu64 ") then (%u->%u, %" PRIu64 ")",
              i, i + 1, rawr.rows[i].from_event, rawr.rows[i].to_event,
              rawr.rows[i].count, rawr.rows[i + 1].from_event,
              rawr.rows[i + 1].to_event, rawr.rows[i + 1].count);
    }
    CHECK(order_ties >= 9, "§3c saw only %d tied adjacent raw rows — without "
          "ties this check cannot see an unordered comparator", order_ties);
    /* And the two orders agree row for row, which is what makes a bit-exact
     * row comparison between the paths possible at all. */
    for (int i = 0; i < rawr.num_rows && i < spn2; i++)
        CHECK(rawr.rows[i].from_event == sp2[i].from_event &&
              rawr.rows[i].to_event == sp2[i].to_event,
              "§3c row %d differs: raw %u->%u, aggregate %u->%u", i,
              rawr.rows[i].from_event, rawr.rows[i].to_event,
              sp2[i].from_event, sp2[i].to_event);
    free(rawr.rows);
    free(sp2);

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
    int n = collect_window(pair_blocks, 2, 0, UINT64_MAX, 1, all,
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
                    /* A MERGEd block must be wholly inside under the SAME
                     * bound the records are selected by (C7: inclusive on the
                     * END key) — stated here independently of plan()'s code. */
                    CHECK(b[k].id.first_timestamp_ns >= from &&
                          b[k].id.last_timestamp_ns <= to,
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
                    /* THE CONVERSE, and the assertion M20 showed was missing:
                     * a block wholly inside the window WITH an aggregate
                     * available must MERGE. Without this, plan() could
                     * degrade to "always DECODE" and every number in this
                     * file would still be right while the phase did nothing
                     * at all — correct and worthless, which no other check
                     * here can tell apart from correct and fast. */
                    CHECK(!(b[k].id.first_timestamp_ns >= from &&
                            b[k].id.last_timestamp_ns <= to),
                          "DECODE of a block wholly inside [%" PRIu64
                          ", %" PRIu64 "] that had an aggregate: the fast "
                          "path declined work it could have done", from, to);
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
            int n = collect_window(b, nb, from, to, 1, raw,
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

/* ── §6b the END-KEY bound, pinned with a literal in both directions ────── */

static void section6b_inclusive_end_divergence(void)
{
    printf("=== §6b the end-key bound: agreement with the loader, and the "
           "cost of getting it wrong ===\n");

    /* The loader selects `ts in [from, to]` because timestamp_ns is when the
     * wait ENDED — an event ending exactly at `to` lies wholly inside the
     * window. This module matches that (C7). Both directions are pinned:
     *   - against the loader's own predicate: ZERO divergence;
     *   - against a HALF-OPEN end bound: short by exactly the records at
     *     ts == to, which is 1 record and 163 ns on this fixture.
     * The second half is why this section exists. An earlier draft read the
     * plan's "half-open throughout" as applying to the END key, made the
     * loader match, and zeroed test_data_aas (Total AAS 4.0 -> 0) and
     * test_data_categories on the box. The number below is what that costs. */
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

    /* The loader's own predicate: the merged answer must match it exactly. */
    CHECK(rp_incl.res.total_transitions == win.total_transitions,
          "the merged answer must match the LOADER's inclusive-end selection "
          "exactly: raw=%" PRIu64 " merged=%" PRIu64,
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
    CHECK(c_raw == c_merged && t_raw == t_merged,
          "CPU->IO1 must match the loader exactly: raw=(%" PRIu64 ", %" PRIu64
          ") merged=(%" PRIu64 ", %" PRIu64 ")", c_raw, t_raw, c_merged,
          t_merged);

    /* The other direction: a HALF-OPEN end bound is short by exactly the
     * ts == to record. This literal makes the cost of the wrong convention a
     * number rather than an argument. */
    struct pgwt_trace_event raw_ho[FIX_MAX_EV * FIX_MAX_BLOCKS];
    int n_ho = collect_window(b, nb, from, to, 0, raw_ho,
                              (int)(sizeof(raw_ho) / sizeof(raw_ho[0])));
    struct raw_pairs rp_ho;
    raw_pairs_of(&rp_ho, raw_ho, n_ho);
    CHECK(rp_ho.res.total_transitions == win.total_transitions - 1,
          "a half-open end bound must LOSE exactly one transition here: "
          "half-open=%" PRIu64 " merged=%" PRIu64,
          rp_ho.res.total_transitions, win.total_transitions);
    uint64_t c_ho = 0, t_ho = 0;
    for (int i = 0; i < rp_ho.res.num_rows; i++)
        if (rp_ho.res.rows[i].from_event == E_CPU &&
            rp_ho.res.rows[i].to_event == E_IO1) {
            c_ho = rp_ho.res.rows[i].count;
            t_ho = (uint64_t)rp_ho.res.rows[i].total_ns;
        }
    CHECK(c_ho == c_merged - 1 && t_ho == t_merged - 163,
          "and the loss is exactly that record's 163 ns: half-open=(%" PRIu64
          ", %" PRIu64 ") merged=(%" PRIu64 ", %" PRIu64 ")",
          c_ho, t_ho, c_merged, t_merged);

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
    int n = collect_window(b, nb, from, to, 1, raw,
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
    /* from == to is a ONE-INSTANT window, not an empty one: with selection
     * inclusive at both ends (C7), a record whose wait ended exactly at that
     * instant belongs to it. Only an INVERTED window selects nothing. The
     * block here spans [1000, 1100], so the instant 1000 is inside it. */
    CHECK(pgwt_block_agg_plan(&b.id, 1, 1000, 1000) == PGWT_BLOCK_DECODE,
          "B12 a one-instant window inside a block must DECODE, not SKIP it "
          "and not merge it whole");
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
        int n = collect_window(&nq, 1, 0, UINT64_MAX, 1, raw, FIX_MAX_EV);
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


/* ── §9 on-disk: the SAME iteration the server runs, against raw ─────────
 *
 * §1-§8 work on in-memory fixtures. This section writes a real multi-block
 * trace with the real writer, reads it back with the real reader, and drives
 * pgwt_block_agg_window_from_reader() — the exact function src/server.c's
 * handle_transitions() calls. So what is compared here is the shipped
 * plan/merge/decode sequence, not a restatement of it.
 *
 * The cache contract is the other half: the sweep runs TWICE over the same
 * windows. Pass 1 starts cold, so every block DECODEs and the aggregates get
 * built and stored. Pass 2 finds them and MERGEs. Both passes must agree with
 * raw AND with each other, to the nanosecond — "a cached aggregate answers
 * identically to a fresh decode" is the entire reason the cache is allowed to
 * exist, and if it did not hold, the fast path would return a different number
 * the second time you looked at the same window.
 */

#define SEC9_CACHE_MAX 64

struct sec9_cache {
    struct pgwt_block_agg agg[SEC9_CACHE_MAX];
    int used[SEC9_CACHE_MAX];
    int n;
    long lookups, hits, stores, rejected;
};

static const struct pgwt_block_agg *
sec9_lookup(void *ctx, const struct pgwt_block_identity *id)
{
    struct sec9_cache *c = ctx;
    c->lookups++;
    for (int i = 0; i < SEC9_CACHE_MAX; i++) {
        if (!c->used[i])
            continue;
        /* Revalidate exactly as the server's hook does: an entry that cannot
         * be proven to still describe this block is not returned. */
        if (pgwt_block_agg_matches(&c->agg[i], id)) {
            c->hits++;
            return &c->agg[i];
        }
    }
    return NULL;
}

static void sec9_store(void *ctx, struct pgwt_block_agg *agg)
{
    struct sec9_cache *c = ctx;
    for (int i = 0; i < SEC9_CACHE_MAX; i++) {
        if (c->used[i])
            continue;
        c->used[i] = 1;
        c->agg[i] = *agg;
        memset(agg, 0, sizeof(*agg));   /* ownership moved */
        c->n++;
        c->stores++;
        return;
    }
    c->rejected++;                      /* full: declining is not an error */
}

static void sec9_cache_free(struct sec9_cache *c)
{
    for (int i = 0; i < SEC9_CACHE_MAX; i++)
        if (c->used[i])
            pgwt_block_agg_free(&c->agg[i]);
    memset(c, 0, sizeof(*c));
}

/* A never-matching lookup: the plan must then DECODE every block and still
 * produce the right answer. This is the "cache that cannot see" case. */
static const struct pgwt_block_agg *
sec9_lookup_blind(void *ctx, const struct pgwt_block_identity *id)
{
    (void)ctx; (void)id;
    return NULL;
}


/* A decode hook that counts its calls and otherwise behaves exactly like the
 * built-in path. §9d uses it to prove the hook is REACHED and that routing the
 * decode through it changes nothing about the answer — the property the
 * server's current-trace-cache hook relies on. */
struct sec9_decode_ctx {
    struct sec9_cache cache;
    struct pgwt_trace_event buf[PGWT_BLOCK_EVENTS];
    long calls;
};

static const struct pgwt_block_agg *
sec9d_lookup(void *ctx, const struct pgwt_block_identity *id)
{
    return sec9_lookup(&((struct sec9_decode_ctx *)ctx)->cache, id);
}

static void sec9d_store(void *ctx, struct pgwt_block_agg *agg)
{
    sec9_store(&((struct sec9_decode_ctx *)ctx)->cache, agg);
}

static int sec9d_decode(void *ctx, struct pgwt_event_reader *r, int block_idx,
                        const struct pgwt_trace_event **out,
                        struct pgwt_block_info *bi)
{
    struct sec9_decode_ctx *d = ctx;
    d->calls++;
    int n = pgwt_reader_decode_block_info(r, block_idx, d->buf,
                                          PGWT_BLOCK_EVENTS, bi);
    if (n < 0)
        return -1;
    *out = d->buf;
    return n;
}

static int sec9_write_trace(const char *dir, int n_blocks, int with_samples,
                            uint64_t *first_ts, uint64_t *last_ts)
{
    struct pgwt_event_writer w;
    memset(&w, 0, sizeof(w));
    if (pgwt_writer_init(&w, dir, 170004, 0, NULL) != 0)
        return -1;
    uint64_t ts = 1700000000000000000ULL;
    *first_ts = ts;
    static const uint32_t froms[] = { E_CPU, E_IO1, E_LW1, E_LOCK1, E_CLIR,
                                      E_IO2 };
    static const uint32_t tos[]   = { E_IO1, E_CPU, E_LW1, E_IO2, E_LOCK1 };
    int k = 0;
    for (int b = 0; b < n_blocks; b++) {
        for (int i = 0; i < PGWT_BLOCK_EVENTS; i++) {
            struct pgwt_trace_event e;
            memset(&e, 0, sizeof(e));
            e.timestamp_ns = ts;
            e.pid = 100 + (uint32_t)(k % 7);
            e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
            /* Every seventh record is structural or invisible, so markers,
             * EXIT and Activity-class endpoints all cross the window edges. */
            if (k % 7 == 3) {
                e.old_event = PGWT_MARKER_EXEC_START;
                e.new_event = PGWT_MARKER_EXEC_START;
            } else if (k % 11 == 5) {
                e.old_event = E_IO1;
                e.new_event = PGWT_EVENT_EXIT;
                e.duration_ns = 1000 + (uint64_t)(k % 997);
            } else if (k % 13 == 7) {
                e.old_event = E_HIDE;
                e.new_event = E_CPU;
                e.duration_ns = 2000 + (uint64_t)(k % 101);
            } else {
                e.old_event = froms[k % 6];
                e.new_event = tos[k % 5];
                e.duration_ns = 101 + (uint64_t)(k % 9173);
            }
            e.query_id = 900 + (uint64_t)(k % 3);
            if (pgwt_writer_push_event(&w, &e) != 0) {
                pgwt_writer_close(&w);
                return -1;
            }
            ts += 1000;
            k++;
        }
    }
    if (with_samples) {
        /* One SAMPLES block AFTER the transition blocks. Its records sit past
         * every transition record, so a window that stops before it must still
         * be eligible, and one that reaches it must be refused. */
        struct pgwt_trace_event smp[8];
        for (int i = 0; i < 8; i++) {
            memset(&smp[i], 0, sizeof(smp[i]));
            smp[i].timestamp_ns = ts + (uint64_t)i * 1000;
            smp[i].pid = 200;
            smp[i].new_event = E_IO1;
            smp[i].query_id = 7;
            smp[i].cpu_ns = PGWT_CPU_NS_UNKNOWN;
        }
        if (pgwt_writer_push_samples(&w, smp, 8, 100000000ULL) != 0) {
            pgwt_writer_close(&w);
            return -1;
        }
        ts += 8 * 1000;
    }
    *last_ts = ts - 1000;
    return pgwt_writer_close(&w);
}

/* The one trace file the writer produced in `dir`. */
static int sec9_find_trace(const char *dir, char *out, size_t outsz)
{
    struct pgwt_trace_file_entry entries[8];
    int n = pgwt_scan_trace_files(dir, entries, 8);
    if (n <= 0)
        return -1;
    snprintf(out, outsz, "%s", entries[n - 1].path);
    return 0;
}

/* Decode every block of the file into one array — the raw side's input. */
static int sec9_decode_all(const char *path, struct pgwt_trace_event **out)
{
    struct pgwt_event_reader r;
    if (pgwt_reader_open(&r, path) != 0)
        return -1;
    int cap = r.num_blocks * PGWT_BLOCK_EVENTS + 64, n = 0;
    struct pgwt_trace_event *all = calloc((size_t)cap, sizeof(*all));
    if (!all) { pgwt_reader_close(&r); return -1; }
    for (int b = 0; b < r.num_blocks; b++) {
        struct pgwt_block_info bi;
        int got = pgwt_reader_decode_block_info(&r, b, all + n, cap - n, &bi);
        if (got < 0) { free(all); pgwt_reader_close(&r); return -1; }
        /* SAMPLES records come back carrying the reader's SAMPLE flag, exactly
         * as the server's loader sees them, so the raw side rejects them for
         * the same reason the aggregate side does. */
        n += got;
    }
    pgwt_reader_close(&r);
    *out = all;
    return n;
}

static void section9_on_disk(void)
{
    printf("=== §9 on-disk: pgwt_block_agg_window_from_reader vs raw, twice "
           "===\n");

    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/pgwt_bagg_sec9_%d", (int)getpid());
    char rm[320];
    snprintf(rm, sizeof(rm), "rm -rf '%s'", dir);
    if (system(rm) != 0) { /* nothing to remove yet */ }
    if (mkdir(dir, 0700) != 0) {
        CHECK(0, "§9 could not create %s", dir);
        return;
    }

    uint64_t first_ts = 0, last_ts = 0;
    if (sec9_write_trace(dir, 4, 0, &first_ts, &last_ts) != 0) {
        CHECK(0, "§9 could not write the trace fixture");
        if (system(rm) != 0) { }
        return;
    }
    char path[512];
    if (sec9_find_trace(dir, path, sizeof(path)) != 0) {
        CHECK(0, "§9 could not find the written trace");
        if (system(rm) != 0) { }
        return;
    }

    struct pgwt_trace_event *all = NULL;
    int n_all = sec9_decode_all(path, &all);
    CHECK(n_all == 4 * PGWT_BLOCK_EVENTS,
          "§9 expected %d records on disk, decoded %d",
          4 * PGWT_BLOCK_EVENTS, n_all);
    if (n_all <= 0) { free(all); if (system(rm) != 0) { } return; }

    /* Block bounds, so windows can land exactly on them. */
    struct pgwt_event_reader r0;
    CHECK(pgwt_reader_open(&r0, path) == 0, "§9 reopen");
    uint64_t bnd[64];
    int nbnd = 0;
    for (int b = 0; b < r0.num_blocks && nbnd < 56; b++) {
        struct pgwt_block_info bi;
        if (pgwt_reader_block_info(&r0, b, &bi) != 0)
            continue;
        uint64_t v[5] = { bi.first_timestamp_ns - 1, bi.first_timestamp_ns,
                          bi.first_timestamp_ns + 1000,
                          bi.last_timestamp_ns, bi.last_timestamp_ns + 1 };
        for (int i = 0; i < 5; i++) {
            int dup = 0;
            for (int j = 0; j < nbnd; j++) if (bnd[j] == v[i]) { dup = 1; break; }
            if (!dup) bnd[nbnd++] = v[i];
        }
    }
    int n_blocks_on_disk = r0.num_blocks;
    pgwt_reader_close(&r0);
    CHECK(n_blocks_on_disk == 4, "§9 expected 4 blocks, got %d",
          n_blocks_on_disk);
    for (int i = 1; i < nbnd; i++)
        for (int j = i; j > 0 && bnd[j - 1] > bnd[j]; j--) {
            uint64_t t = bnd[j]; bnd[j] = bnd[j - 1]; bnd[j - 1] = t;
        }

    struct sec9_cache cache;
    memset(&cache, 0, sizeof(cache));
    long windows = 0, merges = 0, decodes = 0, pass2_merges = 0;

    for (int pass = 1; pass <= 2; pass++) {
        for (int i = 0; i < nbnd; i++) {
            for (int j = i + 1; j < nbnd; j++) {
                uint64_t from = bnd[i], to = bnd[j];

                struct pgwt_event_reader r;
                CHECK(pgwt_reader_open(&r, path) == 0, "§9 open in sweep");
                struct pgwt_block_agg win;
                pgwt_block_agg_init_window(&win);
                uint64_t exact = 0;
                int m = 0, d = 0;
                int rc = pgwt_block_agg_window_from_reader(
                    &r, from, to, &win, sec9_lookup, sec9_store, NULL, &cache,
                    &exact, &m, &d);
                pgwt_reader_close(&r);
                CHECK(rc == PGWT_BAGG_OK, "§9 pass %d window refused rc=%d",
                      pass, rc);
                if (rc != PGWT_BAGG_OK) { pgwt_block_agg_free(&win); continue; }

                /* raw side: the shipping pair implementation plus the
                 * independent node oracle, over the same window (C7:
                 * INCLUSIVE at both ends, not half-open). */
                struct pgwt_trace_event *sel =
                    calloc((size_t)n_all, sizeof(*sel));
                int ns = 0;
                for (int k2 = 0; k2 < n_all; k2++)
                    if (all[k2].timestamp_ns >= from &&
                        all[k2].timestamp_ns <= to)
                        sel[ns++] = all[k2];
                struct raw_pairs rp;
                raw_pairs_of(&rp, sel, ns);
                struct node_oracle_row nrows[64];
                int nn = raw_node_oracle(sel, ns, nrows, 64);

                int bad = compare_agg_vs_raw(&win, &rp.res, nrows, nn, 0);
                if (bad) {
                    fprintf(stderr, "§9 pass %d window [%" PRIu64 ", %" PRIu64
                            ") merge=%d decode=%d:\n", pass, from, to, m, d);
                    compare_agg_vs_raw(&win, &rp.res, nrows, nn, 1);
                }
                CHECK(bad == 0, "§9 pass %d window [%" PRIu64 ", %" PRIu64
                      "): %d disagreement(s) with raw", pass, from, to, bad);
                CHECK(exact == (uint64_t)ns,
                      "§9 exact-record count %" PRIu64 " != %d admitted",
                      exact, ns);

                windows++;
                merges += m;
                decodes += d;
                if (pass == 2) pass2_merges += m;

                raw_pairs_free(&rp);
                free(sel);
                pgwt_block_agg_free(&win);
            }
        }
    }
    printf("    %ld windows x 2 passes, %ld merges (%ld in pass 2), "
           "%ld decodes, cache: %ld lookups / %ld hits / %ld stores\n",
           windows / 2, merges, pass2_merges, decodes,
           cache.lookups, cache.hits, cache.stores);
    CHECK(pass2_merges > 0,
          "§9 pass 2 never MERGEd a cached block — the cache contract was "
          "never exercised, so agreement proves only that decoding works");
    CHECK(decodes > 0, "§9 never DECODEd a block");
    CHECK(cache.hits > 0, "§9 the cache was never hit");

    /* A cache that can never answer: every block DECODEs, and the result must
     * be identical. "No aggregate" is a complete answer, not a degraded one. */
    {
        uint64_t from = bnd[0], to = bnd[nbnd - 1];
        struct pgwt_event_reader ra, rb;
        struct pgwt_block_agg wa, wb;
        uint64_t ea = 0, eb = 0;
        int ma = 0, da = 0, mb = 0, db = 0;
        CHECK(pgwt_reader_open(&ra, path) == 0, "§9 blind open a");
        pgwt_block_agg_init_window(&wa);
        CHECK(pgwt_block_agg_window_from_reader(&ra, from, to, &wa,
              sec9_lookup_blind, NULL, NULL, NULL, &ea, &ma, &da) == PGWT_BAGG_OK,
              "§9 blind-cache window");
        pgwt_reader_close(&ra);
        CHECK(pgwt_reader_open(&rb, path) == 0, "§9 blind open b");
        pgwt_block_agg_init_window(&wb);
        CHECK(pgwt_block_agg_window_from_reader(&rb, from, to, &wb,
              sec9_lookup, sec9_store, NULL, &cache, &eb, &mb, &db) == PGWT_BAGG_OK,
              "§9 warm-cache window");
        pgwt_reader_close(&rb);
        CHECK(ma == 0 && da > 0,
              "§9 a blind cache must DECODE everything (merged=%d decoded=%d)",
              ma, da);
        CHECK(mb > 0, "§9 the warm cache must MERGE (merged=%d)", mb);
        CHECK(wa.total_transitions == wb.total_transitions &&
              wa.pair_total_ns == wb.pair_total_ns &&
              wa.node_total_ns == wb.node_total_ns &&
              wa.n_pairs == wb.n_pairs && wa.n_nodes == wb.n_nodes &&
              ea == eb,
              "§9 blind-cache and warm-cache answers differ: "
              "(%" PRIu64 ", %" PRIu64 ", %" PRIu64 ") vs (%" PRIu64
              ", %" PRIu64 ", %" PRIu64 ")",
              wa.total_transitions, wa.pair_total_ns, wa.node_total_ns,
              wb.total_transitions, wb.pair_total_ns, wb.node_total_ns);
        seen.refusals_observed++;
        pgwt_block_agg_free(&wa);
        pgwt_block_agg_free(&wb);
    }

    sec9_cache_free(&cache);
    free(all);
    if (system(rm) != 0) { }

    /* A SAMPLES block that overlaps the window must be REFUSED, not skipped:
     * the tables would be right and the fidelity label wrong, and a wrong
     * label on a right number is still a wrong answer. One outside the window
     * is skipped, and that window still answers. */
    {
        char sdir[256];
        snprintf(sdir, sizeof(sdir), "/tmp/pgwt_bagg_sec9s_%d", (int)getpid());
        char srm[320];
        snprintf(srm, sizeof(srm), "rm -rf '%s'", sdir);
        if (system(srm) != 0) { }
        CHECK(mkdir(sdir, 0700) == 0, "§9 mkdir samples dir");
        uint64_t f2 = 0, l2 = 0;
        CHECK(sec9_write_trace(sdir, 1, 1, &f2, &l2) == 0,
              "§9 write samples fixture");
        char spath[512];
        if (sec9_find_trace(sdir, spath, sizeof(spath)) == 0) {
            struct pgwt_event_reader r;
            CHECK(pgwt_reader_open(&r, spath) == 0, "§9 open samples fixture");
            /* Window reaching the end: the SAMPLES block overlaps. */
            struct pgwt_block_agg w1;
            pgwt_block_agg_init_window(&w1);
            uint64_t e1 = 0; int m1 = 0, d1 = 0;
            int rc1 = pgwt_block_agg_window_from_reader(&r, f2, l2 + 1, &w1,
                          sec9_lookup_blind, NULL, NULL, NULL, &e1, &m1, &d1);
            CHECK(rc1 == PGWT_BAGG_REFUSED_BLOCK_TYPE,
                  "§9 an overlapping SAMPLES block must be REFUSED, got %d",
                  rc1);
            pgwt_block_agg_free(&w1);
            seen.refusals_observed++;
            /* Window stopping before the samples: eligible, and answers. */
            struct pgwt_block_agg w2;
            pgwt_block_agg_init_window(&w2);
            uint64_t e2 = 0; int m2 = 0, d2 = 0;
            int rc2 = pgwt_block_agg_window_from_reader(&r, f2,
                          f2 + 1000ULL * 64, &w2, sec9_lookup_blind, NULL,
                          NULL, NULL, &e2, &m2, &d2);
            CHECK(rc2 == PGWT_BAGG_OK,
                  "§9 a SAMPLES block OUTSIDE the window must be skipped, "
                  "not refused (got %d)", rc2);
            /* 65, not 64: the window end is inclusive (C7), so the record at
             * exactly f2 + 64*1000 is admitted along with the 64 before it. */
            CHECK(e2 == 65, "§9 expected 65 admitted records, got %" PRIu64,
                  e2);
            pgwt_block_agg_free(&w2);

            /* §9c-bis THE TRIPWIRE Phase 2 makes live, pinned here.
             *
             * The non-TRANSITIONS refusal inside
             * pgwt_block_agg_window_from_reader() used to test overlap with a
             * HALF-OPEN `bi.first_timestamp_ns < to_mono_ns` while C7 is
             * INCLUSIVE at both ends. So a SAMPLES block whose FIRST record
             * ends exactly at `to` was not "overlapping" and was silently
             * SKIPPED — the aggregate answering a window the raw path would
             * have labelled MIXED, with no refusal, no fidelity change and
             * numbers short by the whole sampled contribution.
             *
             * It was unreachable in Phase 1 only because src/server.c's
             * bagg_window_eligible() refused any window a SAMPLES block
             * touched. Phase 2 is the work that lifts that refusal, which is
             * exactly when an unreachable off-by-one becomes a shipped
             * silent-wrong answer. The predicate is now the shared
             * pgwt_block_agg_file_can_contribute(), and this is the case that
             * tells the two readings apart: with `<` the call returns OK,
             * with the inclusive rule it REFUSES.
             *
             * The SAMPLES block begins one tick after the last transition
             * record, i.e. at f2 + PGWT_BLOCK_EVENTS * 1000. */
            {
                uint64_t samples_first =
                    f2 + (uint64_t)PGWT_BLOCK_EVENTS * 1000ULL;
                struct pgwt_block_agg w3;
                pgwt_block_agg_init_window(&w3);
                uint64_t e3 = 0; int m3 = 0, d3 = 0;
                int rc3 = pgwt_block_agg_window_from_reader(&r, f2,
                              samples_first, &w3, sec9_lookup_blind, NULL,
                              NULL, NULL, &e3, &m3, &d3);
                CHECK(rc3 == PGWT_BAGG_REFUSED_BLOCK_TYPE,
                      "§9c-bis a SAMPLES block whose first record ends "
                      "exactly at `to` must be REFUSED (C7 is inclusive), "
                      "got rc=%d with %" PRIu64 " records admitted — a "
                      "half-open `first < to` reading returns OK here and "
                      "answers a MIXED window as if it were EXACT",
                      rc3, e3);
                pgwt_block_agg_free(&w3);
                seen.refusals_observed++;
                /* One tick earlier the SAMPLES block genuinely cannot
                 * contribute, and the window must still ANSWER — otherwise
                 * the fix above would be a blanket refusal wearing the shape
                 * of a boundary rule. */
                struct pgwt_block_agg w4;
                pgwt_block_agg_init_window(&w4);
                uint64_t e4 = 0; int m4 = 0, d4 = 0;
                int rc4 = pgwt_block_agg_window_from_reader(&r, f2,
                              samples_first - 1, &w4, sec9_lookup_blind, NULL,
                              NULL, NULL, &e4, &m4, &d4);
                CHECK(rc4 == PGWT_BAGG_OK,
                      "§9c-bis one tick before the SAMPLES block the window "
                      "must still answer, got rc=%d", rc4);
                CHECK(e4 > 0, "§9c-bis that window admitted nothing");
                pgwt_block_agg_free(&w4);
            }
            pgwt_reader_close(&r);
        }
        if (system(srm) != 0) { }
    }

    /* §9d THE DECODE HOOK. src/server.c supplies one so a boundary block of
     * current.trace is read through the #283 decoded-block cache instead of
     * being re-decompressed — which is also what keeps that cache's
     * served/decoded counters meaningful for a request the fast path
     * answered. Two things must hold, and neither is observable from the
     * numbers alone: the hook is actually REACHED, and routing the decode
     * through it changes NOTHING about the answer. */
    {
        char hdir[256];
        snprintf(hdir, sizeof(hdir), "/tmp/pgwt_bagg_sec9d_%d", (int)getpid());
        char hrm[320];
        snprintf(hrm, sizeof(hrm), "rm -rf '%s'", hdir);
        if (system(hrm) != 0) { }
        CHECK(mkdir(hdir, 0700) == 0, "§9d mkdir");
        uint64_t f4 = 0, l4 = 0;
        CHECK(sec9_write_trace(hdir, 2, 0, &f4, &l4) == 0, "§9d write");
        char hpath[512];
        if (sec9_find_trace(hdir, hpath, sizeof(hpath)) == 0) {
            /* A window that straddles both blocks' interiors, so at least one
             * block must DECODE and therefore reach the hook. */
            uint64_t from = f4 + 1000ULL * 10;
            uint64_t to   = l4 - 1000ULL * 10;

            struct pgwt_event_reader ra;
            struct pgwt_block_agg wa;
            uint64_t ea = 0; int ma = 0, da = 0;
            CHECK(pgwt_reader_open(&ra, hpath) == 0, "§9d open built-in");
            pgwt_block_agg_init_window(&wa);
            CHECK(pgwt_block_agg_window_from_reader(&ra, from, to, &wa,
                      sec9_lookup_blind, NULL, NULL, NULL, &ea, &ma, &da)
                  == PGWT_BAGG_OK, "§9d built-in decode");
            pgwt_reader_close(&ra);

            struct sec9_decode_ctx d;
            memset(&d, 0, sizeof(d));
            struct pgwt_event_reader rb;
            struct pgwt_block_agg wb;
            uint64_t eb = 0; int mb = 0, db = 0;
            CHECK(pgwt_reader_open(&rb, hpath) == 0, "§9d open hooked");
            pgwt_block_agg_init_window(&wb);
            CHECK(pgwt_block_agg_window_from_reader(&rb, from, to, &wb,
                      sec9d_lookup, sec9d_store, sec9d_decode, &d,
                      &eb, &mb, &db) == PGWT_BAGG_OK, "§9d hooked decode");
            pgwt_reader_close(&rb);

            CHECK(da > 0, "§9d the window must DECODE at least one block, "
                  "or the hook is unreachable and this proves nothing");
            CHECK(d.calls == db,
                  "§9d the hook must be called once per DECODEd block "
                  "(calls=%ld decoded=%d)", d.calls, db);
            CHECK(d.calls > 0, "§9d the decode hook was never reached");
            CHECK(ea == eb && wa.total_transitions == wb.total_transitions &&
                  wa.pair_total_ns == wb.pair_total_ns &&
                  wa.node_total_ns == wb.node_total_ns &&
                  wa.n_pairs == wb.n_pairs && wa.n_nodes == wb.n_nodes,
                  "§9d a decode hook must not change the answer: "
                  "built-in (%" PRIu64 ", %" PRIu64 ") vs hooked (%" PRIu64
                  ", %" PRIu64 ")", wa.total_transitions, wa.pair_total_ns,
                  wb.total_transitions, wb.pair_total_ns);
            CHECK(wa.pair_total_ns > 0,
                  "§9d both answers were empty — the comparison is vacuous");
            pgwt_block_agg_free(&wa);
            pgwt_block_agg_free(&wb);
            sec9_cache_free(&d.cache);
        }
        if (system(hrm) != 0) { }
    }

    /* A file that cannot be opened must refuse, never answer zero. */
    {
        struct pgwt_event_reader r;
        CHECK(pgwt_reader_open(&r, "/nonexistent/pgwt/no-such.trace") != 0,
              "§9 opening a missing trace must fail");
        seen.refusals_observed++;
    }
}


/* §10's oracle wrapper, forward-declared: §9e (below) and §10 both call it,
 * and it is defined with §10's other Phase-2 helpers. */
static void raw_top_events_of(const struct pgwt_trace_event *ev, int n,
                              const char *class_name, uint32_t event_id,
                              uint64_t from, uint64_t to,
                              struct pgwt_events_result *out);

/* ── §9e ON-DISK, through the SHIPPED window builder, with the per-event
 *      table ON ─────────────────────────────────────────────────────────────
 *
 * §10b drives answer_window_phase2(), which MIRRORS
 * pgwt_block_agg_window_from_reader()'s REFUSED_CLIP -> DECODE downgrade.
 * A mirror cannot pin the thing it mirrors: a change that made the shipped
 * function SKIP a clip-refused block instead of decoding it would lose those
 * records and §10b would stay green, because §10b never calls it. So this
 * section drives the shipped function itself, over a real multi-block trace
 * written by the real writer, and compares against pgwt_compute_top_events()
 * over the same file's decoded records.
 *
 * The on-disk fixture's durations run to ~9.3 us against a 1 us record
 * spacing, so most waits start several records before they end and the clip
 * fires densely — which is why this is the fixture that proves the downgrade
 * rather than a synthetic one built to make it fire once. */
static void section9e_on_disk_events(void)
{
    printf("=== §9e on-disk: the SHIPPED window builder with per-event "
           "totals, vs pgwt_compute_top_events ===\n");
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/pgwt_bagg_sec9e_%d", (int)getpid());
    char rm[320];
    snprintf(rm, sizeof(rm), "rm -rf '%s'", dir);
    if (system(rm) != 0) { }
    if (mkdir(dir, 0700) != 0) { CHECK(0, "§9e mkdir"); return; }

    uint64_t f0 = 0, l0 = 0;
    if (sec9_write_trace(dir, 3, 0, &f0, &l0) != 0) {
        CHECK(0, "§9e write");
        if (system(rm) != 0) { }
        return;
    }
    char path[512];
    if (sec9_find_trace(dir, path, sizeof(path)) != 0) {
        CHECK(0, "§9e find");
        if (system(rm) != 0) { }
        return;
    }
    struct pgwt_trace_event *all = NULL;
    int nall = sec9_decode_all(path, &all);
    CHECK(nall > 0, "§9e decoded %d records", nall);
    if (nall <= 0) { free(all); if (system(rm) != 0) { } return; }

    struct sec9_cache cache;
    memset(&cache, 0, sizeof(cache));
    long clip_blocks = 0, merges = 0, decodes = 0, rows = 0;
    uint64_t span = l0 - f0;
    /* Two passes so the second runs against a WARM cache and therefore
     * actually reaches the merge path (and thus the clip pre-flight). */
    for (int pass = 0; pass < 2; pass++) {
        (void)pass;
        for (int i = 0; i <= 8; i++) {
            for (int j = i; j <= 8; j++) {
                uint64_t from = f0 + span * (uint64_t)i / 8;
                uint64_t to   = f0 + span * (uint64_t)j / 8;
                if (to <= from)
                    continue;
                struct pgwt_event_reader r;
                CHECK(pgwt_reader_open(&r, path) == 0, "§9e open");
                struct pgwt_block_agg acc;
                pgwt_block_agg_init_window(&acc);
                CHECK(pgwt_block_agg_enable_events(&acc, from, to) ==
                      PGWT_BAGG_OK, "§9e enable");
                int m = 0, d = 0;
                uint64_t ex = 0;
                int rc = pgwt_block_agg_window_from_reader(
                    &r, from, to, &acc, sec9_lookup, sec9_store, NULL,
                    &cache, &ex, &m, &d);
                CHECK(rc == PGWT_BAGG_OK, "§9e window refused rc=%d", rc);
                merges += m;
                decodes += d;
                clip_blocks += acc.clip_decoded_blocks;

                struct pgwt_trace_event *sel =
                    calloc((size_t)nall, sizeof(*sel));
                int nsel = 0;
                for (int k = 0; k < nall; k++)
                    if (all[k].timestamp_ns >= from &&
                        all[k].timestamp_ns <= to)
                        sel[nsel++] = all[k];
                struct pgwt_events_result res;
                raw_top_events_of(sel, nsel, NULL, 0, from, to, &res);

                struct pgwt_block_agg_event *er = NULL;
                int ner = 0;
                int rrc = pgwt_block_agg_events_sorted(&acc, &er, &ner);
                CHECK(rrc == PGWT_BAGG_OK, "§9e readout rc=%d", rrc);
                CHECK(ner == res.num_rows,
                      "§9e [%" PRIu64 ", %" PRIu64 "] %d aggregate rows vs "
                      "%d oracle rows", from, to, ner, res.num_rows);
                for (int k = 0; k < res.num_rows; k++) {
                    struct pgwt_block_agg_event mine;
                    if (!pgwt_block_agg_event_lookup(&acc,
                            res.rows[k].event_id, &mine)) {
                        CHECK(0, "§9e event %u missing from the aggregate",
                              res.rows[k].event_id);
                        continue;
                    }
                    CHECK(mine.count == res.rows[k].count,
                          "§9e [%" PRIu64 ", %" PRIu64 "] event %u count "
                          "agg=%" PRIu64 " raw=%" PRIu64, from, to,
                          res.rows[k].event_id, mine.count,
                          res.rows[k].count);
                    CHECK((double)mine.total_ns / 1e6 == res.rows[k].total_ms,
                          "§9e [%" PRIu64 ", %" PRIu64 "] event %u total "
                          "agg=%" PRIu64 " ns raw=%.9f ms", from, to,
                          res.rows[k].event_id, mine.total_ns,
                          res.rows[k].total_ms);
                    CHECK(mine.exact_count == res.rows[k].exact_count,
                          "§9e event %u exact_count agg=%" PRIu64 " raw=%"
                          PRIu64, res.rows[k].event_id, mine.exact_count,
                          res.rows[k].exact_count);
                    CHECK((double)mine.max_ns / 1000.0 == res.rows[k].max_us,
                          "§9e event %u max agg=%.6f raw=%.6f",
                          res.rows[k].event_id,
                          (double)mine.max_ns / 1000.0, res.rows[k].max_us);
                    rows++;
                }
                uint64_t dbt = 0;
                CHECK(pgwt_block_agg_db_time_ns(&acc, &dbt) == PGWT_BAGG_OK,
                      "§9e db_time");
                CHECK((double)dbt / 1e6 == res.db_time_ms,
                      "§9e [%" PRIu64 ", %" PRIu64 "] db_time agg=%" PRIu64
                      " raw=%.9f ms", from, to, dbt, res.db_time_ms);
                free(er);
                free(res.rows);
                free(sel);
                pgwt_block_agg_free(&acc);
                pgwt_reader_close(&r);
            }
        }
    }
    printf("    merges=%ld decodes=%ld clip-downgrades=%ld rows=%ld\n",
           merges, decodes, clip_blocks, rows);
    CHECK(merges > 0, "§9e the shipped builder never MERGEd a block, so the "
          "clip pre-flight was never reached through it");
    CHECK(clip_blocks > 0,
          "§9e the shipped builder never downgraded a clip-refused block to "
          "DECODE — the one path a mirror test cannot pin");
    CHECK(rows > 400, "§9e only %ld rows compared", rows);
    seen.ev_shipped_rows_compared = rows;
    seen.ev_shipped_clip_downgrades = clip_blocks;
    sec9_cache_free(&cache);
    free(all);
    if (system(rm) != 0) { }
}

/* ══ §10 Phase 2: per-event and per-class totals ══════════════════════════
 *
 * The oracle is pgwt_compute_top_events() — the SHIPPING function the Events
 * tab is drawn from — called on exactly the records a raw loader would have
 * admitted for the window (C7, inclusive at both ends) with a zeroed filter.
 * Not a restatement of it: a restatement would agree with a shared mistake,
 * and C10's clip is precisely the kind of rule two copies get wrong the same
 * way (it is the #316/#317 family).
 *
 * WHAT MAKES THIS NON-VACUOUS, stated before the code so it can be checked
 * against it: the only interesting record for the clip is one whose wait
 * STARTED before the window opened, because that is the only case where the
 * unclipped per-block total differs from the clipped answer. §0's
 * ev_clipped_records and ev_merge_refused_clip count those, FAIL the run at
 * zero, and §10f falsifies them by re-running the whole comparison over a
 * fixture with every duration set to 1 ns and asserting both go to zero
 * there. A counter that cannot go to zero is not measuring the fixture.
 */

/* ── the Phase-2 fixture ──────────────────────────────────────────────────
 * Deliberately NOT the model fixture of §5-§7: every duration there is around
 * 100 ns, so no wait starts more than 200 ns before its own end and the
 * window clip would fire only in a sliver nothing else reaches. This one
 * carries, by construction:
 *   - a 40 ms wait ending in block 1, so a window opening inside it makes a
 *     block that is WHOLLY INSIDE by end timestamp still need clipping, which
 *     is exactly the PGWT_BAGG_REFUSED_CLIP -> DECODE downgrade (C10);
 *   - IO:1 and IO:2, two events in the SAME class, so the per-class fold is
 *     not a relabelling of the per-event table (§0 class_rows_multi);
 *   - an IO_WORKER record: C9 drops it, C4 and C5 keep it;
 *   - a SAMPLE record with a POSITIVE duration: C9 keeps it, C4 and C5 drop
 *     it, and C12 refuses it for the latency columns;
 *   - Client:ClientRead, idle-but-visible, so db_time is not the row sum;
 *   - a marker, a hidden endpoint and an EXIT;
 *   - durations spread over 13 of the 16 latency buckets.
 */
#define E_TMO WEI(PG_WAIT_TIMEOUT, 1)

static int build_p2_fixture(struct fix_block *b, int max_blocks, int flatten)
{
    assert(max_blocks >= 5);
    memset(b, 0, sizeof(*b) * (size_t)max_blocks);
    int nb = 0;
    /* `flatten` is §10f's falsification lever: every duration becomes ZERO,
     * so no record has an interval at all and nothing can straddle any
     * window boundary. Same shape, same events, same timestamps, same
     * boundary generator — only the one property the clip counters claim to
     * measure is removed, and they must then read zero.
     *
     * NOT 1 ns, which was the first attempt and did not falsify anything:
     * the boundary set contains every record's own END timestamp, so a
     * window opening at `ts` clips a 1 ns record that ended at `ts` (its
     * start is ts-1, before the window) and the counters stayed nonzero.
     * That is the falsification working — it rejected a lever that did not
     * remove the property — and it is why the lever is zero. */
#define D(x) ((uint64_t)(flatten ? 0 : (x)))

    /* Block 0: short waits across the bottom latency buckets. */
    ev_set(&b[nb], 1000000000ULL, 11, E_CPU,  E_IO1,  D(500),   0);
    ev_set(&b[nb], 1000002000ULL, 11, E_IO1,  E_CPU,  D(1500),  0);
    ev_set(&b[nb], 1000010000ULL, 12, E_IO2,  E_CPU,  D(3000),  0);
    ev_set(&b[nb], 1000050000ULL, 12, E_IO1,  E_HIDE, D(20000), 0);
    ev_set(&b[nb], 1000060000ULL, 11, E_HIDE, E_CPU,  D(4000),  0);
    fix_seal(&b[nb], &TRACE_A, 0, 1024); nb++;

    /* Block 1: the long waits. The 40 ms one starts 970.3 ms into the trace,
     * well before this block's own first record, which is the whole point. */
    ev_set(&b[nb], 1010000000ULL, 13, E_LOCK1, E_CPU,   D(5000000),  0);
    ev_set(&b[nb], 1010100000ULL, 13, E_CPU,   E_IO1,   D(300000),   0);
    ev_set(&b[nb], 1010200000ULL, 14, E_CLIR,  E_CPU,   D(2000000),  0);
    ev_set(&b[nb], 1010300000ULL, 15, E_IO2,   E_LOCK1, D(40000000), 0);
    fix_seal(&b[nb], &TRACE_A, 1, 2048); nb++;

    /* Block 2: a marker, an IO_WORKER record and a SAMPLE record — the three
     * records where C9 and C4/C5 disagree. */
    ev_set(&b[nb], 1020000000ULL, 11, PGWT_MARKER_EXEC_START,
           PGWT_MARKER_EXEC_START, 0, 0);
    ev_set(&b[nb], 1020010000ULL, 11, E_CPU, E_IO1, D(7000),
           PGWT_EVENT_FLAG_IO_WORKER);
    ev_set(&b[nb], 1020020000ULL, 11, E_IO1, E_CPU, D(12000),
           PGWT_EVENT_FLAG_SAMPLE);
    ev_set(&b[nb], 1020040000ULL, 16, E_LW1, E_LW1, D(60000), 0);
    fix_seal(&b[nb], &TRACE_A, 2, 3072); nb++;

    /* Block 3: one record, so a window can hold it whole or not at all. */
    ev_set(&b[nb], 1030000000ULL, 17, E_CPU, E_IO1, D(100000), 0);
    fix_seal(&b[nb], &TRACE_A, 3, 4096); nb++;

    /* Block 4: two records at the same instant, an EXIT, and a Timeout event
     * so a fifth class exists. */
    ev_set(&b[nb], 1040000000ULL, 18, E_IO1, PGWT_EVENT_EXIT, D(900000),  0);
    ev_set(&b[nb], 1040000000ULL, 19, E_IO1, E_CPU,           D(1100000), 0);
    ev_set(&b[nb], 1040100000ULL, 18, E_CPU, E_IO2,           D(250000),  0);
    ev_set(&b[nb], 1040200000ULL, 19, E_TMO, E_CPU,           D(16000000), 0);
    fix_seal(&b[nb], &TRACE_A, 4, 5120); nb++;
#undef D
    return nb;
}

/* Window boundaries for the Phase-2 sweep. On top of §6's set (block bounds
 * +-1 and every record timestamp) this adds every record's wait START +-1,
 * because the clip fires at `from` crossing a wait's start and nowhere else:
 * a boundary set built only from END timestamps would sweep sixty windows and
 * never exercise the rule this section exists for. */
static int p2_boundaries(const struct fix_block *b, int nb,
                         uint64_t *out, int max_out)
{
    int n = 0;
    for (int i = 0; i < nb; i++) {
        uint64_t v[2] = { b[i].id.first_timestamp_ns,
                          b[i].id.last_timestamp_ns };
        for (int k = 0; k < 2; k++) {
            for (int d = -1; d <= 1; d++) {
                uint64_t c = v[k] + (uint64_t)(int64_t)d;
                int dup = 0;
                for (int j = 0; j < n; j++) if (out[j] == c) { dup = 1; break; }
                if (!dup) { assert(n < max_out); out[n++] = c; }
            }
        }
        for (int e = 0; e < b[i].n; e++) {
            uint64_t ts = b[i].ev[e].timestamp_ns;
            uint64_t st = ts - b[i].ev[e].duration_ns;
            uint64_t c[4] = { ts, st - 1, st, st + 1 };
            for (int k = 0; k < 4; k++) {
                int dup = 0;
                for (int j = 0; j < n; j++) if (out[j] == c[k]) { dup = 1; break; }
                if (!dup) { assert(n < max_out); out[n++] = c[k]; }
            }
        }
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && out[j - 1] > out[j]; j--) {
            uint64_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return n;
}

/* Build the window answer the PHASE-2 way: the same plan/merge/decode
 * partition as answer_window_phase1(), plus the per-event window declaration
 * and the REFUSED_CLIP -> DECODE downgrade that
 * pgwt_block_agg_window_from_reader() performs. Mirrors the shipped function
 * deliberately; §9e then drives the shipped one itself on a real file so this
 * mirror cannot be the only thing tested. */
static int answer_window_phase2(const struct fix_block *b, int nb,
                                const struct pgwt_block_agg *per_block,
                                const int *have_agg,
                                uint64_t from, uint64_t to,
                                struct pgwt_block_agg *out,
                                int *n_merge, int *n_decode, int *n_clip)
{
    pgwt_block_agg_init_window(out);
    int rc = pgwt_block_agg_enable_events(out, from, to);
    if (rc != PGWT_BAGG_OK)
        return rc;
    *n_merge = *n_decode = *n_clip = 0;
    for (int i = 0; i < nb; i++) {
        enum pgwt_block_plan p =
            pgwt_block_agg_plan(&b[i].id, have_agg[i], from, to);
        if (p == PGWT_BLOCK_SKIP)
            continue;
        if (p == PGWT_BLOCK_MERGE) {
            rc = pgwt_block_agg_merge(out, &per_block[i]);
            if (rc == PGWT_BAGG_REFUSED_CLIP) {
                (*n_clip)++;
                p = PGWT_BLOCK_DECODE;      /* the downgrade, C10 */
            } else if (rc != PGWT_BAGG_OK) {
                return rc;
            } else {
                (*n_merge)++;
                continue;
            }
        }
        (*n_decode)++;
        for (int e = 0; e < b[i].n; e++) {
            if (!pgwt_block_agg_in_window(&b[i].ev[e], from, to))
                continue;
            rc = pgwt_block_agg_add_event(out, &b[i].ev[e]);
            if (rc != PGWT_BAGG_OK)
                return rc;
        }
    }
    return PGWT_BAGG_OK;
}

/* The oracle. wall_ms only feeds `aas`, which nothing here compares. */
static void raw_top_events_of(const struct pgwt_trace_event *ev, int n,
                              const char *class_name, uint32_t event_id,
                              uint64_t from, uint64_t to,
                              struct pgwt_events_result *out)
{
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    if (class_name)
        snprintf(f.class_name, sizeof(f.class_name), "%s", class_name);
    f.event_id = event_id;
    pgwt_compute_top_events(ev, n, &f, from, to, 1000.0, out);
}

/* §10a the C9 predicate, differentially against the shipping function.
 * One record at a time: if pgwt_compute_top_events() emits a row for it, the
 * record counts; if it emits none, it does not. No restatement anywhere. */
static void section10a_event_predicate(void)
{
    printf("=== §10a the per-event predicate vs pgwt_compute_top_events ===\n");
    struct { const char *what; uint32_t from, to; uint64_t ts, dur;
             uint32_t flags; } cases[] = {
        { "plain CPU->IO",            E_CPU,  E_IO1, 50000, 400, 0 },
        { "IO->CPU",                  E_IO1,  E_CPU, 50000, 400, 0 },
        { "hidden old_event",         E_HIDE, E_CPU, 50000, 400, 0 },
        { "hidden new_event",         E_IO1,  E_HIDE,50000, 400, 0 },
        { "new_event EXIT",           E_IO1,  PGWT_EVENT_EXIT, 50000, 400, 0 },
        { "marker old_event",         PGWT_MARKER_EXEC_START,
                                      PGWT_MARKER_EXEC_START, 50000, 0, 0 },
        { "marker new_event only",    E_IO1,  PGWT_MARKER_EXEC_END, 50000,
                                      400, 0 },
        { "SAMPLE flagged",           E_IO1,  E_CPU, 50000, 400,
                                      PGWT_EVENT_FLAG_SAMPLE },
        { "SAMPLE_CONT flagged",      E_IO1,  E_CPU, 50000, 400,
                                      PGWT_EVENT_FLAG_SAMPLE |
                                      PGWT_EVENT_FLAG_SAMPLE_CONT },
        { "IO_WORKER flagged",        E_IO1,  E_CPU, 50000, 400,
                                      PGWT_EVENT_FLAG_IO_WORKER },
        { "ClientRead (idle, visible)", E_CLIR, E_CPU, 50000, 400, 0 },
        { "self edge",                E_LW1,  E_LW1, 50000, 400, 0 },
        { "duration > own timestamp", E_IO1,  E_CPU, 400, 500, 0 },
        { "duration == own timestamp",E_IO1,  E_CPU, 500, 500, 0 },
        { "zero duration",            E_IO1,  E_CPU, 50000, 0, 0 },
        { "marker CMD_END old_event", PGWT_MARKER_CMD_END, E_CPU, 50000,
                                      0, 0 },
        { "IO_WORKER + SAMPLE",       E_IO1,  E_CPU, 50000, 400,
                                      PGWT_EVENT_FLAG_SAMPLE |
                                      PGWT_EVENT_FLAG_IO_WORKER },
    };
    int n = (int)(sizeof(cases) / sizeof(cases[0]));
    int disagreements = 0;
    for (int i = 0; i < n; i++) {
        struct fix_block b;
        memset(&b, 0, sizeof(b));
        ev_set(&b, cases[i].ts, 77, cases[i].from, cases[i].to,
               cases[i].dur, cases[i].flags);
        struct pgwt_events_result res;
        raw_top_events_of(b.ev, 1, NULL, 0, 0, 0, &res);
        int oracle_counts = res.num_rows > 0 ? 1 : 0;
        if (oracle_counts)
            CHECK(res.rows[0].event_id == cases[i].from,
                  "§10a %s: the oracle keyed the row on %u, not old_event %u",
                  cases[i].what, res.rows[0].event_id, cases[i].from);
        int mine = pgwt_block_agg_event_counts(&b.ev[0]) ? 1 : 0;
        if (mine != oracle_counts)
            disagreements++;
        CHECK(mine == oracle_counts,
              "§10a %s: pgwt_block_agg_event_counts says %d, "
              "pgwt_compute_top_events says %d", cases[i].what, mine,
              oracle_counts);
        free(res.rows);
    }
    CHECK(disagreements == 0, "§10a %d predicate disagreement(s)",
          disagreements);
    /* The comparison must have seen BOTH answers, or it is a constant. */
    int yes = 0, no = 0;
    for (int i = 0; i < n; i++) {
        struct fix_block b;
        memset(&b, 0, sizeof(b));
        ev_set(&b, cases[i].ts, 77, cases[i].from, cases[i].to,
               cases[i].dur, cases[i].flags);
        if (pgwt_block_agg_event_counts(&b.ev[0])) yes++; else no++;
    }
    CHECK(yes >= 5 && no >= 5,
          "§10a the case table must contain both answers (%d counted, "
          "%d refused) or agreement is trivial", yes, no);
    seen.ev_predicate_cases = n;
}

/* §10b THE MODEL: per-event rows, per-class fold, DB Time and the two
 * filtered readouts, bit-exact against the shipping function over every
 * window the fixture's boundaries generate. */
#define P2_MAX_B 64
#define P2_MAX_REC 64

struct p2_window_stats { long clipped_records; long clip_refusals; };

static void p2_compare_one_window(const struct fix_block *b, int nb,
                                  const struct pgwt_block_agg *per_block,
                                  const int *have_agg,
                                  uint64_t from, uint64_t to,
                                  struct p2_window_stats *st, const char *tag)
{
    struct pgwt_trace_event raw[P2_MAX_REC];
    int nraw = collect_window(b, nb, from, to, 1, raw, P2_MAX_REC);

    struct pgwt_block_agg acc;
    int m = 0, d = 0, c = 0;
    int rc = answer_window_phase2(b, nb, per_block, have_agg, from, to,
                                  &acc, &m, &d, &c);
    CHECK(rc == PGWT_BAGG_OK, "%s window [%" PRIu64 ", %" PRIu64
          "] refused rc=%d", tag, from, to, rc);
    if (rc != PGWT_BAGG_OK) { pgwt_block_agg_free(&acc); return; }
    st->clip_refusals += c;

    /* How many admitted records actually needed clipping. This is the one
     * number that says the window is interesting at all. */
    for (int i = 0; i < nraw; i++)
        if (pgwt_block_agg_event_counts(&raw[i]) &&
            pgwt_block_agg_clip_ns(&raw[i], from, to) != raw[i].duration_ns)
            st->clipped_records++;

    struct pgwt_events_result res;
    raw_top_events_of(raw, nraw, NULL, 0, from, to, &res);

    struct pgwt_block_agg_event *rows = NULL;
    int nrows = 0;
    int rrc = pgwt_block_agg_events_sorted(&acc, &rows, &nrows);
    CHECK(rrc == PGWT_BAGG_OK,
          "%s the per-event readout must be AVAILABLE, got %d (a refusal "
          "read as 'nothing to compare' is how this gate goes blind)",
          tag, rrc);

    CHECK(nrows == res.num_rows,
          "%s window [%" PRIu64 ", %" PRIu64 "]: %d aggregate rows vs %d "
          "oracle rows", tag, from, to, nrows, res.num_rows);

    for (int i = 0; i < res.num_rows; i++) {
        const struct pgwt_event_row *o = &res.rows[i];
        struct pgwt_block_agg_event mine;
        if (!pgwt_block_agg_event_lookup(&acc, o->event_id, &mine)) {
            CHECK(0, "%s window [%" PRIu64 ", %" PRIu64 "]: event %u is in "
                  "the oracle and ABSENT from the aggregate", tag, from, to,
                  o->event_id);
            continue;
        }
        CHECK(mine.count == o->count,
              "%s [%" PRIu64 ", %" PRIu64 "] event %u count agg=%" PRIu64
              " raw=%" PRIu64, tag, from, to, o->event_id, mine.count,
              o->count);
        CHECK((double)mine.total_ns / 1e6 == o->total_ms,
              "%s [%" PRIu64 ", %" PRIu64 "] event %u total agg=%" PRIu64
              " ns (%.9f ms) raw=%.9f ms", tag, from, to, o->event_id,
              mine.total_ns, (double)mine.total_ns / 1e6, o->total_ms);
        CHECK(mine.exact_count == o->exact_count,
              "%s [%" PRIu64 ", %" PRIu64 "] event %u exact_count agg=%"
              PRIu64 " raw=%" PRIu64, tag, from, to, o->event_id,
              mine.exact_count, o->exact_count);
        CHECK((double)mine.max_ns / 1000.0 == o->max_us,
              "%s [%" PRIu64 ", %" PRIu64 "] event %u max agg=%.6f us "
              "raw=%.6f us", tag, from, to, o->event_id,
              (double)mine.max_ns / 1000.0, o->max_us);
        if (mine.exact_count > 0) {
            double avg = (double)mine.exact_total_ns /
                         (double)mine.exact_count / 1000.0;
            CHECK(avg == o->avg_us,
                  "%s [%" PRIu64 ", %" PRIu64 "] event %u avg agg=%.9f us "
                  "raw=%.9f us", tag, from, to, o->event_id, avg, o->avg_us);
        }
        /* The histogram's population must equal the exact count it was built
         * from, or the percentiles the wiring step derives are drawn from a
         * different population than the row claims. */
        uint64_t hpop = 0;
        for (int k = 0; k < HISTOGRAM_BUCKETS; k++)
            hpop += mine.hist[k];
        CHECK(hpop == mine.exact_count,
              "%s [%" PRIu64 ", %" PRIu64 "] event %u histogram population %"
              PRIu64 " != exact_count %" PRIu64, tag, from, to, o->event_id,
              hpop, mine.exact_count);
        CHECK((hpop > 0) == (o->has_latency_dist != 0),
              "%s [%" PRIu64 ", %" PRIu64 "] event %u has_latency_dist "
              "agg=%d raw=%d", tag, from, to, o->event_id, hpop > 0,
              o->has_latency_dist);

        /* The EVENT-FILTERED readout, which is the Phase-2 claim itself: the
         * aggregate can answer a request filtered to one event by SELECTING
         * its row. Compared against the shipping function given that very
         * filter, so the claim is tested rather than asserted.
         *
         * event_id 0 is EXCLUDED, and that is a property of the product, not
         * a convenience: pgwt_filter_matches() reads `f->event_id != 0` as
         * "no event filter", and 0 is the CPU event id. So an event filter
         * CANNOT select CPU — a request filtered to CPU is an unfiltered
         * request. Found by this differential returning two rows where one
         * was expected. Skipping it keeps the comparison honest; pretending
         * the oracle had filtered would compare an unfiltered answer against
         * one row and call the agreement a pass. Reported as a finding. */
        if (o->event_id == 0) {
            seen.ev_filter_unselectable++;
            seen.event_rows_compared++;
            continue;
        }
        struct pgwt_events_result fres;
        raw_top_events_of(raw, nraw, NULL, o->event_id, from, to, &fres);
        CHECK(fres.num_rows == 1,
              "%s event-filtered oracle returned %d rows for event %u",
              tag, fres.num_rows, o->event_id);
        if (fres.num_rows == 1) {
            CHECK(fres.rows[0].count == mine.count &&
                  fres.rows[0].total_ms == (double)mine.total_ns / 1e6,
                  "%s [%" PRIu64 ", %" PRIu64 "] event-FILTERED event %u: "
                  "raw count=%" PRIu64 " total=%.9f vs aggregate count=%"
                  PRIu64 " total=%.9f", tag, from, to, o->event_id,
                  fres.rows[0].count, fres.rows[0].total_ms, mine.count,
                  (double)mine.total_ns / 1e6);
            seen.ev_filtered_rows_compared++;
        }
        free(fres.rows);
        seen.event_rows_compared++;
    }

    /* The documented TOTAL order of the readout: total_ns DESC, event_id ASC.
     * Needed because the oracle's own comparator orders by total only, which
     * is not a total order, so row-by-row byte equality is not meaningful
     * against it — the readout's determinism is asserted directly instead. */
    for (int i = 1; i < nrows; i++)
        CHECK(rows[i - 1].total_ns > rows[i].total_ns ||
              (rows[i - 1].total_ns == rows[i].total_ns &&
               rows[i - 1].event_id < rows[i].event_id),
              "%s per-event rows %d/%d violate (total_ns desc, event_id asc)",
              tag, i - 1, i);

    /* DB Time: the denominator %DB is reported against, which excludes
     * idle-but-visible events. A fold that forgot that exclusion would still
     * agree on every row above. */
    uint64_t dbt = 0;
    int drc = pgwt_block_agg_db_time_ns(&acc, &dbt);
    CHECK(drc == PGWT_BAGG_OK, "%s db_time refused rc=%d", tag, drc);
    CHECK((double)dbt / 1e6 == res.db_time_ms,
          "%s [%" PRIu64 ", %" PRIu64 "] db_time agg=%" PRIu64 " ns (%.9f ms) "
          "raw=%.9f ms", tag, from, to, dbt, (double)dbt / 1e6,
          res.db_time_ms);

    /* C13: the per-class fold, compared against the shipping function given
     * a CLASS FILTER — the other half of the Phase-2 claim. */
    struct pgwt_block_agg_class *cls = NULL;
    int ncls = 0;
    int crc = pgwt_block_agg_classes_sorted(&acc, &cls, &ncls);
    CHECK(crc == PGWT_BAGG_OK, "%s class readout refused rc=%d", tag, crc);
    for (int i = 0; i < ncls; i++) {
        /* Name the class the way the filter does, from an event that is
         * actually in it, so this test carries no index->name table of its
         * own to drift. */
        const char *cname = NULL;
        int members = 0;
        for (int j = 0; j < nrows; j++)
            if (pgwt_wait_class_index(rows[j].event_id) == cls[i].class_index) {
                if (!cname) cname = pgwt_class_name(rows[j].event_id);
                members++;
            }
        CHECK(cname != NULL, "%s class row %d has no member event", tag, i);
        if (!cname) continue;
        if (members >= 2)
            seen.class_rows_multi++;
        struct pgwt_events_result cres;
        raw_top_events_of(raw, nraw, cname, 0, from, to, &cres);
        uint64_t rcount = 0;
        double rtotal = 0;
        for (int j = 0; j < cres.num_rows; j++) {
            rcount += cres.rows[j].count;
            rtotal += cres.rows[j].total_ms;
        }
        CHECK(rcount == cls[i].count,
              "%s [%" PRIu64 ", %" PRIu64 "] class-FILTERED %s count agg=%"
              PRIu64 " raw=%" PRIu64, tag, from, to, cname, cls[i].count,
              rcount);
        /* Summed in the same order the readout folds them (ascending event
         * id within the class) so the double addition is the same addition;
         * total_ns is exact in uint64 and only converted once. */
        uint64_t agg_ns = 0;
        for (int j = 0; j < nrows; j++)
            if (pgwt_wait_class_index(rows[j].event_id) == cls[i].class_index)
                agg_ns += rows[j].total_ns;
        CHECK(agg_ns == cls[i].total_ns,
              "%s class %s fold disagrees with its own rows: %" PRIu64
              " vs %" PRIu64, tag, cname, agg_ns, cls[i].total_ns);
        /* NOT compared as `(double)total_ns / 1e6 == rtotal`: rtotal is a
         * sum of doubles in the oracle's own row order, so that comparison
         * would be an assertion about float addition order, not about this
         * fold. The class total is pinned EXACTLY instead, in integers and
         * transitively: it equals the integer sum of its member rows
         * (asserted just above), and every member row's total_ns was
         * compared against the oracle bit-exactly in the per-event loop. */
        (void)rtotal;
        free(cres.rows);
        seen.class_rows_compared++;
    }
    /* CONSERVATION, with the two sides taken from DIFFERENT places on
     * purpose. A fold that silently DROPPED a class would not be noticed by
     * the per-class loop above, which only ever walks the rows it was given
     * — that is the shape of a check that cannot fail. So the class rows are
     * reconciled against (a) the ORACLE's own row-count sum, which this
     * module did not produce, and (b) ev_total_ns, which is maintained
     * per-record in ev_observe()/ev_add_row() and never derived from the
     * class fold. */
    {
        uint64_t cls_count = 0, cls_ns = 0, oracle_count = 0;
        for (int i = 0; i < ncls; i++) {
            cls_count += cls[i].count;
            cls_ns += cls[i].total_ns;
        }
        for (int i = 0; i < res.num_rows; i++)
            oracle_count += res.rows[i].count;
        CHECK(cls_count == oracle_count,
              "%s [%" PRIu64 ", %" PRIu64 "] the class fold accounts for %"
              PRIu64 " observations, the ORACLE for %" PRIu64
              " — a class was dropped or double-counted", tag, from, to,
              cls_count, oracle_count);
        CHECK(cls_ns == acc.ev_total_ns,
              "%s [%" PRIu64 ", %" PRIu64 "] the class fold accounts for %"
              PRIu64 " ns, the per-record accumulator for %" PRIu64,
              tag, from, to, cls_ns, acc.ev_total_ns);
    }
    for (int i = 1; i < ncls; i++)
        CHECK(cls[i - 1].total_ns > cls[i].total_ns ||
              (cls[i - 1].total_ns == cls[i].total_ns &&
               cls[i - 1].class_index < cls[i].class_index),
              "%s class rows %d/%d violate (total_ns desc, index asc)",
              tag, i - 1, i);

    if (nrows > 0) seen.ev_windows_with_rows++;
    if (m > 0 && d > 0) seen.ev_windows_at_seam++;
    seen.ev_windows_compared++;

    free(cls);
    free(rows);
    free(res.rows);
    pgwt_block_agg_free(&acc);
}

static void section10b_model(void)
{
    printf("=== §10b model: per-event + per-class == pgwt_compute_top_events, "
           "bit-exact, every window ===\n");
    struct fix_block b[P2_MAX_B];
    int nb = build_p2_fixture(b, P2_MAX_B, 0);

    struct pgwt_block_agg per_block[P2_MAX_B];
    int have_agg[P2_MAX_B];
    for (int i = 0; i < nb; i++) {
        int rc = pgwt_block_agg_build(&per_block[i], &b[i].id, b[i].type,
                                      b[i].committed, b[i].ev, b[i].n);
        CHECK(rc == PGWT_BAGG_OK, "§10b build block %d rc=%d", i, rc);
        have_agg[i] = (rc == PGWT_BAGG_OK);
    }

    uint64_t bnd[512];
    int nbnd = p2_boundaries(b, nb, bnd, 512);
    CHECK(nbnd > 40, "§10b only %d boundaries — the sweep is too narrow",
          nbnd);

    struct p2_window_stats st = { 0, 0 };
    for (int i = 0; i < nbnd; i++)
        for (int j = i; j < nbnd; j++)
            p2_compare_one_window(b, nb, per_block, have_agg, bnd[i], bnd[j],
                                  &st, "§10b");

    seen.ev_clipped_records = st.clipped_records;
    seen.ev_merge_refused_clip = st.clip_refusals;
    printf("    windows compared %ld, clipped records %ld, "
           "clip refusals %ld\n", seen.ev_windows_compared,
           st.clipped_records, st.clip_refusals);

    for (int i = 0; i < nb; i++)
        pgwt_block_agg_free(&per_block[i]);
}

/* §10c the latency ladder, pinned against edges LEARNED from the oracle.
 *
 * pgwt_block_agg_duration_bucket() is a third copy of compute.c's static
 * compute_duration_to_bucket(); neither it nor map_reader.c's version is
 * reachable from this link set. A third copy is only safe if something fails
 * when it drifts, so this does not copy the edge table: for each probe
 * duration it asks pgwt_compute_top_events() for a ONE-RECORD window and
 * takes (p50_us, p50_overflow) as that duration's bucket SIGNATURE — two
 * durations share a bucket in the product iff their signatures match (buckets
 * 14 and 15 share an edge and are told apart by the overflow flag). Then it
 * asserts the module's partition of the probe set is the SAME partition.
 */
static void section10c_latency_ladder(void)
{
    printf("=== §10c the latency ladder vs the oracle's own bucket edges "
           "===\n");
    static const uint64_t edges_us[HISTOGRAM_BUCKETS] = {
        1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192,
        16384, 32768,
    };
    uint64_t probe[4 * HISTOGRAM_BUCKETS + 4];
    int np = 0;
    probe[np++] = 0;
    probe[np++] = 1;
    probe[np++] = 999;
    for (int i = 0; i < HISTOGRAM_BUCKETS; i++) {
        uint64_t e = edges_us[i] * 1000ULL;
        probe[np++] = e - 1;
        probe[np++] = e;
        probe[np++] = e + 1;
    }
    double sig_us[sizeof(probe) / sizeof(probe[0])];
    int sig_ovf[sizeof(probe) / sizeof(probe[0])];
    for (int i = 0; i < np; i++) {
        struct fix_block one;
        memset(&one, 0, sizeof(one));
        ev_set(&one, 100000000000ULL, 55, E_IO1, E_CPU, probe[i], 0);
        struct pgwt_events_result res;
        raw_top_events_of(one.ev, 1, NULL, 0, 0, 0, &res);
        CHECK(res.num_rows == 1,
              "§10c the oracle must return one row for duration %" PRIu64
              " (got %d) — a zero-row answer would make this partition "
              "comparison vacuous", probe[i], res.num_rows);
        if (res.num_rows == 1) {
            sig_us[i] = res.rows[0].p50_us;
            sig_ovf[i] = res.rows[0].p50_overflow;
        } else {
            sig_us[i] = -1; sig_ovf[i] = -1;
        }
        free(res.rows);
    }
    int same_pairs = 0, diff_pairs = 0, bad = 0;
    for (int i = 0; i < np; i++)
        for (int j = i + 1; j < np; j++) {
            int oracle_same = (sig_us[i] == sig_us[j] &&
                               sig_ovf[i] == sig_ovf[j]);
            int mine_same = pgwt_block_agg_duration_bucket(probe[i]) ==
                            pgwt_block_agg_duration_bucket(probe[j]);
            if (oracle_same) same_pairs++; else diff_pairs++;
            if (oracle_same != mine_same) {
                bad++;
                CHECK(0, "§10c durations %" PRIu64 " and %" PRIu64
                      ": the product puts them %s, this module puts them %s "
                      "(buckets %u/%u)", probe[i], probe[j],
                      oracle_same ? "together" : "apart",
                      mine_same ? "together" : "apart",
                      pgwt_block_agg_duration_bucket(probe[i]),
                      pgwt_block_agg_duration_bucket(probe[j]));
            }
        }
    CHECK(same_pairs > 0 && diff_pairs > 0,
          "§10c the probe set must contain both same-bucket and "
          "different-bucket pairs (%d/%d)", same_pairs, diff_pairs);
    CHECK(bad == 0, "§10c %d bucket-partition disagreement(s)", bad);
    /* Monotonic, and it must actually reach both ends of the ladder. */
    for (int i = 1; i < np; i++)
        if (probe[i] > probe[i - 1])
            CHECK(pgwt_block_agg_duration_bucket(probe[i]) >=
                  pgwt_block_agg_duration_bucket(probe[i - 1]),
                  "§10c the ladder is not monotonic at %" PRIu64, probe[i]);
    CHECK(pgwt_block_agg_duration_bucket(0) == 0 &&
          pgwt_block_agg_duration_bucket(100ULL * 1000 * 1000) ==
          HISTOGRAM_BUCKETS - 1,
          "§10c the ladder must span bucket 0 to bucket %d",
          HISTOGRAM_BUCKETS - 1);
    seen.ev_bucket_pairs_checked = same_pairs + diff_pairs;
    printf("    %d probe durations, %d pairs partitioned identically\n",
           np, same_pairs + diff_pairs);
}

/* §10f FALSIFICATION of §0's clip counters. Same fixture, same boundary
 * generator, same comparison — with every duration flattened to 1 ns, so no
 * wait can start before any boundary the fixture generates. Both clip
 * counters MUST read zero. A counter that reads nonzero here is counting
 * something other than what §0 claims it counts, and the §0 gate it feeds
 * would then pass on any fixture at all. */
static void section10f_falsify_counters(void)
{
    printf("=== §10f falsify the clip counters: a straddle-free fixture must "
           "drive them to ZERO ===\n");
    struct fix_block b[P2_MAX_B];
    int nb = build_p2_fixture(b, P2_MAX_B, 1);
    struct pgwt_block_agg per_block[P2_MAX_B];
    int have_agg[P2_MAX_B];
    for (int i = 0; i < nb; i++) {
        int rc = pgwt_block_agg_build(&per_block[i], &b[i].id, b[i].type,
                                      b[i].committed, b[i].ev, b[i].n);
        have_agg[i] = (rc == PGWT_BAGG_OK);
    }
    uint64_t bnd[512];
    int nbnd = p2_boundaries(b, nb, bnd, 512);
    struct p2_window_stats st = { 0, 0 };
    long before = seen.ev_windows_compared;
    for (int i = 0; i < nbnd; i++)
        for (int j = i; j < nbnd; j++)
            p2_compare_one_window(b, nb, per_block, have_agg, bnd[i], bnd[j],
                                  &st, "§10f");
    CHECK(seen.ev_windows_compared > before,
          "§10f compared no window — the falsification itself was vacuous");
    CHECK(st.clipped_records == 0,
          "§10f a fixture with 1 ns durations still produced %ld clipped "
          "record(s): ev_clipped_records does not measure straddling, so the "
          "§0 gate it feeds proves nothing", st.clipped_records);
    CHECK(st.clip_refusals == 0,
          "§10f a fixture with 1 ns durations still produced %ld "
          "REFUSED_CLIP(s): ev_merge_refused_clip does not measure the clip "
          "downgrade", st.clip_refusals);
    printf("    straddle-free fixture: clipped=%ld refusals=%ld (both must "
           "be 0)\n", st.clipped_records, st.clip_refusals);
    for (int i = 0; i < nb; i++)
        pgwt_block_agg_free(&per_block[i]);
}

/* ── §11 bypass suite for the v2 gates ──────────────────────────────────
 * One case per way THIS phase's detection can be made unreachable. The
 * true-positive path (the differential goes red when a number is wrong) is
 * §12's job; these are the false negatives: a readout that answers zero
 * instead of refusing, a guard satisfied without looking, a gate that cannot
 * see and approves anyway. */
static void section11_bypass_v2(void)
{
    printf("=== §11 bypass suite: the v2 readouts must REFUSE when they "
           "cannot see ===\n");
    struct fix_block b[P2_MAX_B];
    int nb = build_p2_fixture(b, P2_MAX_B, 0);
    struct pgwt_block_agg blk;
    CHECK(pgwt_block_agg_build(&blk, &b[1].id, b[1].type, b[1].committed,
                               b[1].ev, b[1].n) == PGWT_BAGG_OK,
          "§11 build block 1");

    /* E1. The table was never enabled. Every readout must REFUSE, and must
     * not hand back an empty list that reads as "no events in this window". */
    {
        struct pgwt_block_agg w;
        pgwt_block_agg_init_window(&w);
        for (int e = 0; e < b[1].n; e++)
            CHECK(pgwt_block_agg_add_event(&w, &b[1].ev[e]) == PGWT_BAGG_OK,
                  "§11 E1 add");
        CHECK(pgwt_block_agg_events_available(&w) == 0,
              "§11 E1 an accumulator with no declared window must report the "
              "per-event table UNAVAILABLE");
        CHECK(w.n_events == 0,
              "§11 E1 no per-event row may be built without a declared "
              "window — %d were", w.n_events);
        struct pgwt_block_agg_event *rows = (void *)0x1;
        int n = -1;
        CHECK(pgwt_block_agg_events_sorted(&w, &rows, &n) ==
              PGWT_BAGG_REFUSED_UNAVAILABLE,
              "§11 E1 events_sorted must refuse, not answer empty");
        CHECK(rows == NULL && n == 0,
              "§11 E1 a refused readout must leave no plausible output "
              "behind (rows=%p n=%d)", (void *)rows, n);
        struct pgwt_block_agg_class *cls = (void *)0x1;
        int nc = -1;
        CHECK(pgwt_block_agg_classes_sorted(&w, &cls, &nc) ==
              PGWT_BAGG_REFUSED_UNAVAILABLE,
              "§11 E1 classes_sorted must refuse");
        CHECK(cls == NULL && nc == 0, "§11 E1 class refusal must not leave "
              "output behind");
        uint64_t dbt = 12345;
        CHECK(pgwt_block_agg_db_time_ns(&w, &dbt) ==
              PGWT_BAGG_REFUSED_UNAVAILABLE,
              "§11 E1 db_time must refuse when the table is off");
        CHECK(pgwt_block_agg_event_lookup(&w, E_IO1, NULL) == 0,
              "§11 E1 event_lookup must report ABSENT when the table is off");
        /* The transitions answer must be UNCHANGED by all of this — the v1
         * contract still holds for a Phase-1 caller. */
        CHECK(w.total_transitions > 0,
              "§11 E1 the pair table must still have been built");
        seen.refusals_observed += 5;
        pgwt_block_agg_free(&w);
    }

    /* E2. A declared window, but NOTHING qualifies. This must be OK with zero
     * rows — the "absent rather than wrong" case: an empty answer and a
     * refusal are different facts and must stay distinguishable. */
    {
        struct pgwt_block_agg w;
        pgwt_block_agg_init_window(&w);
        CHECK(pgwt_block_agg_enable_events(&w, 1, 2) == PGWT_BAGG_OK,
              "§11 E2 enable");
        struct pgwt_block_agg_event *rows = (void *)0x1;
        int n = -1;
        CHECK(pgwt_block_agg_events_sorted(&w, &rows, &n) == PGWT_BAGG_OK,
              "§11 E2 an empty-but-available table must be OK, not refused");
        CHECK(rows == NULL && n == 0, "§11 E2 expected zero rows");
        CHECK(pgwt_block_agg_events_available(&w) == 1,
              "§11 E2 available must stay 1 for an empty table");
        pgwt_block_agg_free(&w);
    }

    /* E3. enable_events() after records have been folded in: half the window
     * would be clipped and half not. REFUSE. */
    {
        struct pgwt_block_agg w;
        pgwt_block_agg_init_window(&w);
        CHECK(pgwt_block_agg_add_event(&w, &b[1].ev[0]) == PGWT_BAGG_OK,
              "§11 E3 add");
        CHECK(pgwt_block_agg_enable_events(&w, 1000, 2000) ==
              PGWT_BAGG_REFUSED_MODE,
              "§11 E3 a window may not be retro-fitted onto a half-built "
              "accumulator");
        seen.refusals_observed++;
        pgwt_block_agg_free(&w);
    }

    /* E4. enable_events() on a MODE_BLOCK aggregate, and on MODE_NONE. */
    {
        CHECK(pgwt_block_agg_enable_events(&blk, 1000, 2000) ==
              PGWT_BAGG_REFUSED_MODE,
              "§11 E4 a block aggregate has no window to declare");
        struct pgwt_block_agg none;
        memset(&none, 0, sizeof(none));
        CHECK(pgwt_block_agg_enable_events(&none, 1000, 2000) ==
              PGWT_BAGG_REFUSED_MODE,
              "§11 E4 MODE_NONE must refuse");
        CHECK(pgwt_block_agg_enable_events(NULL, 1000, 2000) ==
              PGWT_BAGG_REFUSED_INVALID, "§11 E4 NULL must refuse");
        CHECK(pgwt_block_agg_events_available(NULL) == 0,
              "§11 E4 availability of NULL is 0");
        CHECK(pgwt_block_agg_events_available(&none) == 0,
              "§11 E4 availability of MODE_NONE is 0");
        seen.refusals_observed += 3;
    }

    /* E5. An INVERTED window would clip every record to zero and look like a
     * legitimately empty window. */
    {
        struct pgwt_block_agg w;
        pgwt_block_agg_init_window(&w);
        CHECK(pgwt_block_agg_enable_events(&w, 5000, 4000) ==
              PGWT_BAGG_REFUSED_INVALID,
              "§11 E5 an inverted window must refuse");
        CHECK(pgwt_block_agg_events_available(&w) == 0,
              "§11 E5 a refused enable must leave the table OFF");
        seen.refusals_observed++;
        pgwt_block_agg_free(&w);
    }

    /* E6. THE CLIP REFUSAL, and that it leaves dst byte-identical. Block 1's
     * 40 ms wait starts at 970.3 ms, so any window opening after that needs
     * clipping even though the block is wholly inside by end timestamp. */
    {
        struct pgwt_block_agg w;
        pgwt_block_agg_init_window(&w);
        CHECK(pgwt_block_agg_enable_events(&w, 1000000000ULL,
                                           1050000000ULL) == PGWT_BAGG_OK,
              "§11 E6 enable");
        CHECK(pgwt_block_agg_plan(&b[1].id, 1, 1000000000ULL,
                                  1050000000ULL) == PGWT_BLOCK_MERGE,
              "§11 E6 the block must be MERGE-eligible by end timestamp, or "
              "this case is not testing the clip refusal at all");
        struct pgwt_block_agg snap = w;
        int rc = pgwt_block_agg_merge(&w, &blk);
        CHECK(rc == PGWT_BAGG_REFUSED_CLIP,
              "§11 E6 a block holding a wait that started before `from` must "
              "be REFUSED_CLIP, got %d", rc);
        CHECK(w.n_events == snap.n_events && w.n_pairs == snap.n_pairs &&
              w.n_keys == snap.n_keys && w.ev_total_ns == snap.ev_total_ns &&
              w.pair_total_ns == snap.pair_total_ns &&
              w.total_transitions == snap.total_transitions,
              "§11 E6 a REFUSED_CLIP must leave the accumulator untouched — "
              "the downgrade to DECODE would otherwise double-count");
        seen.refusals_observed++;
        pgwt_block_agg_free(&w);
    }

    /* E7. The same refusal from the OTHER end: a block holding a record that
     * ends after `to`. plan() would not MERGE it, but merge() is public. */
    {
        struct pgwt_block_agg w;
        pgwt_block_agg_init_window(&w);
        CHECK(pgwt_block_agg_enable_events(&w, 1, 1010100000ULL) ==
              PGWT_BAGG_OK, "§11 E7 enable");
        CHECK(pgwt_block_agg_merge(&w, &blk) == PGWT_BAGG_REFUSED_CLIP,
              "§11 E7 a block reaching past `to` must be REFUSED_CLIP");
        seen.refusals_observed++;
        pgwt_block_agg_free(&w);
    }

    /* E8. A source with the per-event table OFF must never contribute a
     * silent zero to a destination that has it ON. Hand-built because
     * build() always turns it on, which is exactly why the guard needs a
     * test: it protects against a FUTURE caller, and a guard with no test is
     * a guard that will be deleted as unreachable. */
    {
        struct pgwt_block_agg fake;
        CHECK(pgwt_block_agg_build(&fake, &b[3].id, b[3].type, b[3].committed,
                                   b[3].ev, b[3].n) == PGWT_BAGG_OK,
              "§11 E8 build");
        fake.events_on = 0;
        struct pgwt_block_agg w;
        pgwt_block_agg_init_window(&w);
        CHECK(pgwt_block_agg_enable_events(&w, 1, 2000000000ULL) ==
              PGWT_BAGG_OK, "§11 E8 enable");
        CHECK(pgwt_block_agg_merge(&w, &fake) ==
              PGWT_BAGG_REFUSED_UNAVAILABLE,
              "§11 E8 a source with no per-event table must refuse, not "
              "contribute nothing silently");
        CHECK(w.n_pairs == 0 && w.total_transitions == 0,
              "§11 E8 the refusal must be pre-flight");
        seen.refusals_observed++;
        pgwt_block_agg_free(&w);
        pgwt_block_agg_free(&fake);
    }

    /* E9. A VERSION the module did not write. The per-event numbers were
     * computed under a different rule, so they are unreadable, not
     * approximately right. */
    {
        struct pgwt_block_agg old;
        CHECK(pgwt_block_agg_build(&old, &b[3].id, b[3].type, b[3].committed,
                                   b[3].ev, b[3].n) == PGWT_BAGG_OK,
              "§11 E9 build");
        old.version = PGWT_BLOCK_AGG_VERSION - 1;
        CHECK(pgwt_block_agg_events_available(&old) == 0,
              "§11 E9 a stale-version aggregate's per-event table must read "
              "UNAVAILABLE, not be trusted");
        struct pgwt_block_agg_event *rows = (void *)0x1;
        int n = -1;
        CHECK(pgwt_block_agg_events_sorted(&old, &rows, &n) ==
              PGWT_BAGG_REFUSED_UNAVAILABLE, "§11 E9 readout must refuse");
        struct pgwt_block_agg w;
        pgwt_block_agg_init_window(&w);
        CHECK(pgwt_block_agg_enable_events(&w, 1, 2000000000ULL) ==
              PGWT_BAGG_OK, "§11 E9 enable");
        CHECK(pgwt_block_agg_merge(&w, &old) == PGWT_BAGG_REFUSED_VERSION,
              "§11 E9 merging a stale version must refuse");
        CHECK(pgwt_block_agg_matches(&old, &b[3].id) == 0,
              "§11 E9 a stale-version aggregate must not revalidate");
        seen.refusals_observed += 3;
        pgwt_block_agg_free(&w);
        old.version = PGWT_BLOCK_AGG_VERSION;
        pgwt_block_agg_free(&old);
    }

    /* E10. Allocation failure anywhere in the v2 paths is a REFUSAL, never a
     * short answer. Each site is named, so a site that stopped being
     * reachable shows up as a green CHECK on a refusal that never happened —
     * which is why the return code is asserted rather than just "not OK". */
    {
        struct pgwt_block_agg w;
        pgwt_block_agg_init_window(&w);
        CHECK(pgwt_block_agg_enable_events(&w, 1, 2000000000ULL) ==
              PGWT_BAGG_OK, "§11 E10 enable");
        setenv("PGWT_TEST_ALLOC_FAIL", "block_agg_events_grow", 1);
        int rc = pgwt_block_agg_add_event(&w, &b[0].ev[0]);
        unsetenv("PGWT_TEST_ALLOC_FAIL");
        CHECK(rc == PGWT_BAGG_REFUSED_NOMEM,
              "§11 E10 a failed per-event table growth must refuse, got %d",
              rc);
        pgwt_block_agg_free(&w);

        struct pgwt_block_agg w2;
        pgwt_block_agg_init_window(&w2);
        CHECK(pgwt_block_agg_enable_events(&w2, 1, 2000000000ULL) ==
              PGWT_BAGG_OK, "§11 E10 enable 2");
        for (int e = 0; e < b[0].n; e++)
            (void)pgwt_block_agg_add_event(&w2, &b[0].ev[e]);
        CHECK(w2.n_events > 0, "§11 E10 need rows for the readout to fail on");
        struct pgwt_block_agg_event *rows = (void *)0x1;
        int n = -1;
        setenv("PGWT_TEST_ALLOC_FAIL", "block_agg_events_sorted", 1);
        rc = pgwt_block_agg_events_sorted(&w2, &rows, &n);
        unsetenv("PGWT_TEST_ALLOC_FAIL");
        CHECK(rc == PGWT_BAGG_REFUSED_NOMEM,
              "§11 E10 a failed readout allocation must refuse, got %d", rc);
        CHECK(rows == NULL && n == 0,
              "§11 E10 a NOMEM readout must leave no output behind");
        struct pgwt_block_agg_class *cls = (void *)0x1;
        int nc = -1;
        setenv("PGWT_TEST_ALLOC_FAIL", "block_agg_classes_sorted", 1);
        rc = pgwt_block_agg_classes_sorted(&w2, &cls, &nc);
        unsetenv("PGWT_TEST_ALLOC_FAIL");
        CHECK(rc == PGWT_BAGG_REFUSED_NOMEM,
              "§11 E10 a failed class readout must refuse, got %d", rc);
        CHECK(cls == NULL && nc == 0,
              "§11 E10 a NOMEM class readout must leave no output behind");
        seen.refusals_observed += 3;
        pgwt_block_agg_free(&w2);

        /* build() must refuse WHOLE on a per-event allocation failure, not
         * return a block aggregate with pairs and no events. */
        struct pgwt_block_agg partial;
        setenv("PGWT_TEST_ALLOC_FAIL", "block_agg_events_grow", 1);
        rc = pgwt_block_agg_build(&partial, &b[0].id, b[0].type,
                                  b[0].committed, b[0].ev, b[0].n);
        unsetenv("PGWT_TEST_ALLOC_FAIL");
        CHECK(rc == PGWT_BAGG_REFUSED_NOMEM,
              "§11 E10 build must refuse on a per-event NOMEM, got %d", rc);
        CHECK(partial.mode == PGWT_BAGG_MODE_NONE,
              "§11 E10 a refused build must leave MODE_NONE, not a usable "
              "aggregate missing its per-event table");
        seen.refusals_observed++;
    }

    /* E11. The FILTER predicate. A pid or query_id filter has no dimension in
     * the table and must be refused; a class or event filter is a selection
     * on the key and is supported. A predicate that said "yes" to everything
     * would let a pid-filtered request read an unfiltered answer. */
    {
        struct pgwt_filter f;
        memset(&f, 0, sizeof(f));
        CHECK(pgwt_block_agg_event_filter_supported(NULL) == 1,
              "§11 E11 no filter is supported");
        CHECK(pgwt_block_agg_event_filter_supported(&f) == 1,
              "§11 E11 an empty filter is supported");
        memset(&f, 0, sizeof(f));
        snprintf(f.class_name, sizeof(f.class_name), "IO");
        CHECK(pgwt_block_agg_event_filter_supported(&f) == 1,
              "§11 E11 a class filter selects on the key");
        memset(&f, 0, sizeof(f));
        f.event_id = E_IO1;
        CHECK(pgwt_block_agg_event_filter_supported(&f) == 1,
              "§11 E11 an event filter selects on the key");
        memset(&f, 0, sizeof(f));
        f.pid = 42;
        CHECK(pgwt_block_agg_event_filter_supported(&f) == 0,
              "§11 E11 a pid filter has NO dimension in the per-event table");
        memset(&f, 0, sizeof(f));
        f.query_id = 42;
        CHECK(pgwt_block_agg_event_filter_supported(&f) == 0,
              "§11 E11 a query_id filter has no dimension either");
        /* And the v1 predicate must NOT have been widened by v2: the pair and
         * node tables still have no dimension at all. */
        memset(&f, 0, sizeof(f));
        snprintf(f.class_name, sizeof(f.class_name), "IO");
        CHECK(pgwt_block_agg_filter_supported(&f) == 0,
              "§11 E11 the PAIR table must still refuse a class filter — "
              "widening it would answer a filtered transitions request with "
              "unfiltered numbers");
        seen.refusals_observed += 3;
    }

    /* E12. A SAMPLE_CONT record in a BLOCK. pgwt_compute_top_events()' count
     * rule ("first record for this event always counts") is ORDER-dependent,
     * so a per-block table holding a continuation fragment could not be
     * merged without risking a count that depends on block order. Such a
     * record cannot reach a committed TRANSITIONS block (flags are
     * reader-side annotations and C1 refuses SAMPLES blocks), but build() is
     * public, so it REFUSES rather than relying on that argument. */
    {
        struct fix_block c;
        memset(&c, 0, sizeof(c));
        ev_set(&c, 1000000000ULL, 11, E_IO1, E_CPU, 400, 0);
        ev_set(&c, 1000001000ULL, 11, E_IO1, E_CPU, 400,
               PGWT_EVENT_FLAG_SAMPLE | PGWT_EVENT_FLAG_SAMPLE_CONT);
        fix_seal(&c, &TRACE_A, 9, 9216);
        struct pgwt_block_agg a;
        CHECK(pgwt_block_agg_build(&a, &c.id, c.type, c.committed, c.ev, c.n)
              == PGWT_BAGG_REFUSED_BLOCK_TYPE,
              "§11 E12 a block holding a SAMPLE_CONT fragment must be "
              "REFUSED — its per-event count is order-dependent");
        CHECK(a.mode == PGWT_BAGG_MODE_NONE,
              "§11 E12 the refused build must leave MODE_NONE");
        seen.refusals_observed++;
        /* ...and the SAME record through add_event() must be handled, not
         * refused: that is the path src/server.c drives over post-merge
         * records, where continuation fragments are real. */
        struct pgwt_block_agg w;
        pgwt_block_agg_init_window(&w);
        CHECK(pgwt_block_agg_enable_events(&w, 1, 2000000000ULL) ==
              PGWT_BAGG_OK, "§11 E12 enable");
        for (int e = 0; e < c.n; e++)
            CHECK(pgwt_block_agg_add_event(&w, &c.ev[e]) == PGWT_BAGG_OK,
                  "§11 E12 add_event must accept a continuation fragment");
        struct pgwt_block_agg_event got;
        CHECK(pgwt_block_agg_event_lookup(&w, E_IO1, &got) == 1,
              "§11 E12 lookup");
        /* Two records, one physical observation: the fragment adds duration
         * and does not bump the count. Compared against the oracle rather
         * than asserted as a literal. */
        struct pgwt_events_result res;
        raw_top_events_of(c.ev, c.n, NULL, 0, 1, 2000000000ULL, &res);
        CHECK(res.num_rows == 1, "§11 E12 oracle rows %d", res.num_rows);
        if (res.num_rows == 1) {
            CHECK(got.count == res.rows[0].count,
                  "§11 E12 SAMPLE_CONT count agg=%" PRIu64 " raw=%" PRIu64,
                  got.count, res.rows[0].count);
            CHECK((double)got.total_ns / 1e6 == res.rows[0].total_ms,
                  "§11 E12 SAMPLE_CONT total agg=%" PRIu64 " raw=%.9f",
                  got.total_ns, res.rows[0].total_ms);
            CHECK(res.rows[0].count == 1,
                  "§11 E12 the fixture must produce exactly one physical "
                  "observation from two records, or the rule is untested "
                  "(got %" PRIu64 ")", res.rows[0].count);
        }
        free(res.rows);
        pgwt_block_agg_free(&w);
    }

    /* E13. An out-of-range class index must refuse the whole fold rather than
     * clamp into UNKNOWN and report somebody else's time under a name. The
     * only way to reach it is a module-internal invariant, so what is tested
     * here is the boundary itself: every event id the fixture produces maps
     * inside [0, PGWT_NUM_CLASSES). */
    {
        struct pgwt_block_agg w;
        pgwt_block_agg_init_window(&w);
        CHECK(pgwt_block_agg_enable_events(&w, 1, 2000000000ULL) ==
              PGWT_BAGG_OK, "§11 E13 enable");
        for (int i = 0; i < nb; i++)
            for (int e = 0; e < b[i].n; e++)
                (void)pgwt_block_agg_add_event(&w, &b[i].ev[e]);
        struct pgwt_block_agg_event *rows = NULL;
        int n = 0;
        CHECK(pgwt_block_agg_events_sorted(&w, &rows, &n) == PGWT_BAGG_OK,
              "§11 E13 readout");
        int in_range = 0;
        for (int i = 0; i < n; i++) {
            int c = pgwt_wait_class_index(rows[i].event_id);
            CHECK(c >= 0 && c < PGWT_NUM_CLASSES,
                  "§11 E13 event %u maps to class %d, outside [0, %d)",
                  rows[i].event_id, c, PGWT_NUM_CLASSES);
            if (c >= 0 && c < PGWT_NUM_CLASSES) in_range++;
        }
        CHECK(in_range >= 5,
              "§11 E13 only %d event(s) checked — the fixture must reach at "
              "least five classes for the fold to be worth testing",
              in_range);
        free(rows);
        pgwt_block_agg_free(&w);
    }

    pgwt_block_agg_free(&blk);
}

/* ── §12 mutation probes: the v2 comparator can see one wrong number ────
 * §10's comparator is the only thing between a wrong per-event number and a
 * green run, so it is shown going red rather than assumed to. Each probe
 * perturbs ONE field of an otherwise correct answer. */
static void section12_v2_probes(void)
{
    printf("=== §12 mutation probes: the v2 comparator can see one count, "
           "one ns, one row ===\n");
    struct fix_block b[P2_MAX_B];
    int nb = build_p2_fixture(b, P2_MAX_B, 0);
    struct pgwt_block_agg per_block[P2_MAX_B];
    int have_agg[P2_MAX_B];
    for (int i = 0; i < nb; i++) {
        int rc = pgwt_block_agg_build(&per_block[i], &b[i].id, b[i].type,
                                      b[i].committed, b[i].ev, b[i].n);
        have_agg[i] = (rc == PGWT_BAGG_OK);
    }
    uint64_t from = 1000000000ULL, to = 1040200000ULL;
    struct pgwt_trace_event raw[P2_MAX_REC];
    int nraw = collect_window(b, nb, from, to, 1, raw, P2_MAX_REC);
    struct pgwt_events_result res;
    raw_top_events_of(raw, nraw, NULL, 0, from, to, &res);
    CHECK(res.num_rows >= 4,
          "§12 the probe window must hold at least four rows, got %d",
          res.num_rows);

    struct pgwt_block_agg acc;
    int m = 0, d = 0, c = 0;
    CHECK(answer_window_phase2(b, nb, per_block, have_agg, from, to, &acc,
                               &m, &d, &c) == PGWT_BAGG_OK, "§12 answer");

    /* Control: the unperturbed answer agrees on every field. */
    int base_bad = 0;
    for (int i = 0; i < res.num_rows; i++) {
        struct pgwt_block_agg_event mine;
        if (!pgwt_block_agg_event_lookup(&acc, res.rows[i].event_id, &mine)) {
            base_bad++;
            continue;
        }
        if (mine.count != res.rows[i].count) base_bad++;
        if ((double)mine.total_ns / 1e6 != res.rows[i].total_ms) base_bad++;
        if (mine.exact_count != res.rows[i].exact_count) base_bad++;
    }
    CHECK(base_bad == 0, "§12 the control must agree (%d disagreements)",
          base_bad);

    /* P1: one count off by one. */
    {
        struct pgwt_block_agg_event *s = NULL;
        for (int i = 0; i < acc.ev_cap; i++)
            if (acc.events[i].count != 0) { s = &acc.events[i]; break; }
        CHECK(s != NULL, "§12 P1 needs a row to perturb");
        if (s) {
            s->count++;
            int bad = 0;
            for (int i = 0; i < res.num_rows; i++) {
                struct pgwt_block_agg_event mine;
                if (pgwt_block_agg_event_lookup(&acc, res.rows[i].event_id,
                                                &mine) &&
                    mine.count != res.rows[i].count)
                    bad++;
            }
            CHECK(bad > 0, "§12 P1 one extra observation went UNSEEN");
            if (bad > 0) seen.v2_probes_went_red++;
            s->count--;
        }
    }
    /* P2: one nanosecond on one total. */
    {
        struct pgwt_block_agg_event *s = NULL;
        for (int i = 0; i < acc.ev_cap; i++)
            if (acc.events[i].count != 0) { s = &acc.events[i]; break; }
        if (s) {
            s->total_ns += 1;
            int bad = 0;
            for (int i = 0; i < res.num_rows; i++) {
                struct pgwt_block_agg_event mine;
                if (pgwt_block_agg_event_lookup(&acc, res.rows[i].event_id,
                                                &mine) &&
                    (double)mine.total_ns / 1e6 != res.rows[i].total_ms)
                    bad++;
            }
            CHECK(bad > 0, "§12 P2 ONE nanosecond on one event total went "
                  "UNSEEN — the comparison is not bit-exact");
            if (bad > 0) seen.v2_probes_went_red++;
            s->total_ns -= 1;
        }
    }
    /* P3: a whole row removed. The row-count check must catch it. */
    {
        struct pgwt_block_agg_event *s = NULL;
        for (int i = 0; i < acc.ev_cap; i++)
            if (acc.events[i].count != 0) { s = &acc.events[i]; break; }
        if (s) {
            struct pgwt_block_agg_event keep = *s;
            memset(s, 0, sizeof(*s));
            acc.n_events--;
            struct pgwt_block_agg_event *rows = NULL;
            int n = 0;
            CHECK(pgwt_block_agg_events_sorted(&acc, &rows, &n) ==
                  PGWT_BAGG_OK, "§12 P3 readout");
            CHECK(n != res.num_rows,
                  "§12 P3 a MISSING row went unseen (%d == %d)", n,
                  res.num_rows);
            if (n != res.num_rows) seen.v2_probes_went_red++;
            free(rows);
            acc.n_events++;
            *s = keep;
        }
    }
    /* P4: one bucket moved inside a histogram — the population check must see
     * a count that no longer matches its distribution. */
    {
        struct pgwt_block_agg_event *s = NULL;
        for (int i = 0; i < acc.ev_cap; i++)
            if (acc.events[i].exact_count > 0) { s = &acc.events[i]; break; }
        CHECK(s != NULL, "§12 P4 needs a row with a histogram");
        if (s) {
            s->hist[HISTOGRAM_BUCKETS - 1]++;
            uint64_t hpop = 0;
            for (int k = 0; k < HISTOGRAM_BUCKETS; k++) hpop += s->hist[k];
            CHECK(hpop != s->exact_count,
                  "§12 P4 a fabricated histogram entry went UNSEEN");
            if (hpop != s->exact_count) seen.v2_probes_went_red++;
            s->hist[HISTOGRAM_BUCKETS - 1]--;
        }
    }
    /* P5: DB Time must notice an idle event being charged to it. */
    {
        uint64_t before = 0;
        CHECK(pgwt_block_agg_db_time_ns(&acc, &before) == PGWT_BAGG_OK,
              "§12 P5 db_time");
        struct pgwt_block_agg_event idle;
        CHECK(pgwt_block_agg_event_lookup(&acc, E_CLIR, &idle) == 1,
              "§12 P5 the window must contain an idle-but-visible event, or "
              "the DB-Time exclusion is untested");
        CHECK(before > 0, "§12 P5 db_time must be positive");
        CHECK(idle.total_ns > 0, "§12 P5 the idle event must carry time");
        CHECK(before != before + idle.total_ns,
              "§12 P5 the exclusion is not observable");
        CHECK((double)before / 1e6 == res.db_time_ms,
              "§12 P5 db_time agg=%" PRIu64 " raw=%.9f", before,
              res.db_time_ms);
        /* And the exclusion is REAL: adding the idle row back would change
         * the number, so a fold that forgot it would be red. */
        CHECK((double)(before + idle.total_ns) / 1e6 != res.db_time_ms,
              "§12 P5 including the idle event would give the SAME db_time, "
              "so the exclusion is unexercised by this window");
        seen.v2_probes_went_red++;
    }

    free(res.rows);
    pgwt_block_agg_free(&acc);
    for (int i = 0; i < nb; i++)
        pgwt_block_agg_free(&per_block[i]);
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
    section9_on_disk();
    section9e_on_disk_events();
    section10a_event_predicate();
    section10b_model();
    section10c_latency_ladder();
    section10f_falsify_counters();
    section11_bypass_v2();
    section12_v2_probes();

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

    /* Phase 2's own ledger (§10-§12). */
    printf("    §2 predicate cases ................ %ld\n",
           seen.ev_predicate_cases);
    printf("    §2 windows compared ............... %ld\n",
           seen.ev_windows_compared);
    printf("    §2 windows with >=1 event row ..... %ld\n",
           seen.ev_windows_with_rows);
    printf("    §2 windows at a merge/decode seam . %ld\n",
           seen.ev_windows_at_seam);
    printf("    §2 event rows compared ............ %ld\n",
           seen.event_rows_compared);
    printf("    §2 event-FILTERED rows compared ... %ld\n",
           seen.ev_filtered_rows_compared);
    printf("    §2 class rows compared ............ %ld\n",
           seen.class_rows_compared);
    printf("    §2 class rows with >=2 events ..... %ld\n",
           seen.class_rows_multi);
    printf("    §2 records that needed clipping ... %ld\n",
           seen.ev_clipped_records);
    printf("    §2 REFUSED_CLIP -> DECODE ......... %ld\n",
           seen.ev_merge_refused_clip);
    printf("    §2 SHIPPED-builder rows compared ... %ld\n",
           seen.ev_shipped_rows_compared);
    printf("    §2 SHIPPED clip downgrades ........ %ld\n",
           seen.ev_shipped_clip_downgrades);
    printf("    §2 rows whose event is unfilterable %ld\n",
           seen.ev_filter_unselectable);
    printf("    §2 latency-bucket pairs checked ... %ld\n",
           seen.ev_bucket_pairs_checked);
    printf("    §2 v2 probes that went red ........ %ld\n",
           seen.v2_probes_went_red);

    CHECK(seen.ev_predicate_cases > 10,
          "§10a ran %ld predicate cases", seen.ev_predicate_cases);
    CHECK(seen.ev_windows_compared > 100,
          "only %ld per-event windows compared", seen.ev_windows_compared);
    CHECK(seen.ev_windows_with_rows > 50,
          "only %ld windows produced an event row — comparing empty answers "
          "proves nothing", seen.ev_windows_with_rows);
    CHECK(seen.ev_windows_at_seam > 0,
          "no window merged one block AND decoded another while the "
          "per-event table was on — the seam was never tested");
    CHECK(seen.event_rows_compared > 200,
          "only %ld event rows compared", seen.event_rows_compared);
    CHECK(seen.ev_filtered_rows_compared > 200,
          "only %ld EVENT-FILTERED rows compared — the Phase-2 claim itself "
          "was not exercised", seen.ev_filtered_rows_compared);
    CHECK(seen.class_rows_compared > 100,
          "only %ld class rows compared", seen.class_rows_compared);
    CHECK(seen.class_rows_multi > 0,
          "no compared class held two distinct events, so the per-class fold "
          "was only ever a relabelling of the per-event table");
    CHECK(seen.ev_clipped_records > 0,
          "no admitted record ever needed CLIPPING, so C10 — the one rule "
          "that makes per-event totals different from per-event sums — was "
          "never exercised (see §10f, which falsifies this counter)");
    CHECK(seen.ev_merge_refused_clip > 0,
          "no block was ever REFUSED_CLIP and downgraded to DECODE, so the "
          "C10 fallback path was never taken");
    CHECK(seen.ev_shipped_rows_compared > 400,
          "only %ld rows compared through the SHIPPED window builder — "
          "§10b's mirror cannot pin the function it mirrors",
          seen.ev_shipped_rows_compared);
    CHECK(seen.ev_shipped_clip_downgrades > 0,
          "the SHIPPED window builder never downgraded a clip-refused block "
          "to DECODE");
    CHECK(seen.ev_filter_unselectable > 0,
          "no compared row carried event id 0, so the product limitation "
          "this suite documents (an event filter cannot select CPU) was "
          "never actually reached and the skip above is unexercised");
    CHECK(seen.ev_bucket_pairs_checked > 1000,
          "only %ld latency-bucket pairs checked",
          seen.ev_bucket_pairs_checked);
    CHECK(seen.v2_probes_went_red == 5,
          "expected 5 v2 probes to go red, got %ld", seen.v2_probes_went_red);

    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS: test_block_agg\n");
    return 0;
}
