/* test_interval_index.c — the concurrency interval index (paint-latency
 * plan, Phase 4; src/interval_index.{c,h}).
 *
 * WHAT IS BEING PINNED
 * --------------------
 * One window query over a per-block interval index must return the SAME
 * concurrency answer the raw path returns for the same window — peak
 * sessions per bucket, the event that owns each peak, every burst's exact
 * pid list and `bursts_total` — BIT-EXACTLY, with no epsilon, no tolerance,
 * no retry and no sleep anywhere in this file.
 *
 * THE ORACLE IS THE SHIPPING COMPUTATION. pgwt_compute_concurrency()
 * (src/compute.c) is linked in and called on the FULL record array, exactly
 * as src/server.c's handle_concurrency() calls it. The index's answer also
 * comes out of that same function (over the compacted interval array), so
 * what this differential actually tests is the only thing the index decides:
 * WHICH INTERVALS it stores, in WHICH BLOCK, and WHICH ONES a window
 * selects. A mistake in bucketing or burst detection is impossible by
 * construction; a mistake in selection is what the three silent-wrong modes
 * are made of, and that is what is swept here.
 *
 * THE THREE SILENT-WRONG MODES, each with its own literal expectation
 * -------------------------------------------------------------------
 * 1. CLIPPED start_ns. "fx_long" holds one 700 ms wait whose END record
 *    lands in the LAST block while its start lies back in the first. Clipped
 *    to the block start it would contribute to one bucket instead of eight,
 *    so peaks in the earlier buckets drop and its burst onset shifts later.
 *    literal_expectations() asserts all eight buckets by value, and
 *    mutation M01/M02 in tests/interval_index_mutations.py inject exactly
 *    that clip.
 * 2. AN INTERVAL COUNTED TWICE. Peaks survive it (pid dedup inside a
 *    bucket), but the per-pid burst counter double-counts one pid toward the
 *    threshold — so a 3-pid group reads as a 4-pid burst. "fx_main" carries
 *    a 4-pid burst whose four END records can be cut into four different
 *    blocks; every cut must still report bursts_total == 1 with the same
 *    four pids. Mutation M05 duplicates a boundary row.
 * 3. SILENTLY "FIXING" the raw path's exclusion of waits still open at `to`.
 *    "fx_main" has a wait whose end record is past `to`; the raw path does
 *    not select it, so neither may the index. Asserted by value, and
 *    out->excluded_open_past_to proves the case was reached rather than
 *    assumed. Mutation M06 includes it.
 *
 * WHAT MAKES THIS PASS WHILE BROKEN — and what stops it
 * -----------------------------------------------------
 * a. Feeding every fixture as ONE block. Every boundary class then collapses
 *    and modes 1 and 2 are invisible. Stopped by sweeping EVERY cut set of
 *    every fixture, and by a non-vacuity LEDGER whose counters (cross-block
 *    intervals, intervals starting before coverage, same pid in two chunks
 *    inside one bucket, intervals excluded for ending past `to`, bursts
 *    whose members live in two or more blocks, bursts_total > 0, a peak > 1)
 *    each FAIL the test when zero. Falsified deliberately: run this binary
 *    as `./test_interval_index oneblock` and it feeds every fixture as a
 *    single block and MUST exit nonzero on those counters.
 * b. Vacuity. "index == oracle" is satisfied by two empty answers. Stopped
 *    by the same ledger, plus a fixture with NO waits that must REFUSE (and
 *    whose oracle answer is checked to carry no signal, so the refusal is
 *    not hiding one).
 * c. A comparator that cannot see. Stopped by comparator_probes(): every
 *    compared field is perturbed by the smallest possible amount on a real
 *    answer and the comparator must name that field.
 * d. A gate that cannot establish an answer but approves anyway. Stopped by
 *    bypass_suite(): a NULL index, a NULL filter, an empty index, an
 *    unsealed one, a wrong version, a skipped block, a reordered block, an
 *    out-of-order record, a record outside its block bounds, inverted block
 *    bounds, a window outside coverage, an inverted window, num_buckets 0,
 *    a FILTERED request, a window selecting nothing, a hand-corrupted row
 *    order and every injected allocation failure must each REFUSE with their
 *    own reason and leave have_result == 0. "No index" must never read as
 *    "no concurrency".
 *
 * WHAT DEPENDS ON TIMING OR ORDERING. Nothing wall-clock. Every fixture is a
 * literal array; every expectation is a literal constant or the oracle's own
 * answer. Record ORDER matters (pgwt_compute_concurrency gives a bucket to
 * the first record that REACHES a count) and is pinned by the index refusing
 * a non-monotone feed rather than sorting it. The one timing number printed
 * is REPORTED, never asserted.
 *
 * Pure: links ../src/interval_index.c + ../src/compute.c. No daemon, no
 * PostgreSQL, no root, no files.
 */
#include "interval_index.h"
#include "compute.h"
#include "pg_wait_tracer.h"
#include "wait_event.h"
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

#define MS         1000000ULL
#define BASE       1000000000000ULL
#define T(ms)      (BASE + (uint64_t)(ms) * MS)
/* Coverage deliberately opens at 50 ms, not 0: fx_main's first wait
 * starts at 40 ms, so at least one indexed interval begins BEFORE the
 * indexed coverage itself. Without that the ledger row
 * "intervals started before coverage" is permanently 0 and the
 * unclipped-start rule is only half-exercised. */
#define COVER_FROM T(50)
#define COVER_TO   T(2000)

#define EV_A    (((uint32_t)PG_WAIT_IO << 24) | 21U)
#define EV_B    (((uint32_t)PG_WAIT_IO << 24) | 22U)
#define EV_C    (((uint32_t)PG_WAIT_IO << 24) | 23U)
#define EV_IDLE (((uint32_t)PG_WAIT_ACTIVITY << 24) | 3U)

#define BURST_WIN    (10 * MS)
#define BURST_THRESH 4

/* Deliberate falsification switch (argv[1] == "oneblock"): forces every
 * index build to a SINGLE block so the boundary ledger must go red. */
static int g_force_one_block = 0;

/* ── fixtures ─────────────────────────────────────────────── */

#define MAXEV 20

struct fixture {
    const char *name;
    struct pgwt_trace_event ev[MAXEV];
    int n;
};

static struct pgwt_trace_event wait_rec(int end_ms, uint32_t pid, int dur_ms,
                                        uint32_t event)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = T(end_ms);
    e.duration_ns = (uint64_t)dur_ms * MS;
    e.pid = pid;
    e.old_event = event;      /* the wait being LEFT: this is the interval */
    e.new_event = 0;          /* back on CPU */
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return e;
}

static struct pgwt_trace_event marker_rec(int ts_ms, uint32_t pid,
                                          uint32_t marker)
{
    struct pgwt_trace_event e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ns = T(ts_ms);
    e.pid = pid;
    e.old_event = marker;
    e.new_event = marker;
    e.cpu_ns = PGWT_CPU_NS_UNKNOWN;
    return e;
}

/* on-CPU gap: old_event 0 — not a wait, never part of a burst. */
static struct pgwt_trace_event cpu_rec(int ts_ms, uint32_t pid, int dur_ms)
{
    struct pgwt_trace_event e = wait_rec(ts_ms, pid, dur_ms, 0);
    return e;
}

/* A record whose duration exceeds its own END timestamp: pgwt_filter_matches
 * REFUSES it (never repairs it), so neither path may count it. */
static struct pgwt_trace_event impossible_rec(int ts_ms, uint32_t pid)
{
    struct pgwt_trace_event e = wait_rec(ts_ms, pid, 0, EV_A);
    e.duration_ns = e.timestamp_ns + 1;
    return e;
}

static struct fixture g_fx[10];
static int g_nfx = 0;

static struct fixture *add_fx(const char *name)
{
    struct fixture *f = &g_fx[g_nfx++];
    memset(f, 0, sizeof(*f));
    f->name = name;
    return f;
}

static void push(struct fixture *f, struct pgwt_trace_event e)
{
    f->ev[f->n++] = e;
}

/* Fixture indices, so literal expectations can name one. */
static int FX_MAIN, FX_NOWAIT, FX_ONE, FX_TWOBURST, FX_PIDREPEAT, FX_LONG;
static int FX_EDGE, FX_COVER;

static void build_fixtures(void)
{
    struct fixture *f;

    /* fx_main — every boundary class in one array, ordered by END time.
     * Reference window for the literals: [100 ms, 900 ms], 8 buckets of
     * 100 ms. */
    FX_MAIN = g_nfx;
    f = add_fx("fx_main");
    push(f, wait_rec(100, 41, 60, EV_C));      /* ends AT `from`; starts
                                                * at 40 ms, BEFORE the
                                                * indexed coverage */
    push(f, marker_rec(120, 10, 0xFFFFFFF2U)); /* marker: never a wait */
    push(f, cpu_rec(130, 11, 5));              /* on-CPU: never a wait */
    push(f, wait_rec(140, 42, 20, EV_IDLE));   /* idle wait: excluded */
    push(f, wait_rec(150, 10, 100, EV_A));     /* starts BEFORE the window */
    push(f, impossible_rec(160, 43));          /* refused at the chokepoint */
    push(f, wait_rec(250, 30, 10, EV_A));      /* bucket 1 */
    push(f, wait_rec(280, 30, 10, EV_A));      /* bucket 1 again, SAME pid */
    push(f, wait_rec(600, 20, 100, EV_A));     /* burst onset 500 */
    push(f, wait_rec(620, 21, 118, EV_A));     /* burst onset 502 */
    push(f, wait_rec(700, 22, 196, EV_A));     /* burst onset 504 */
    push(f, wait_rec(800, 23, 294, EV_A));     /* burst onset 506 */
    push(f, wait_rec(900, 40, 50, EV_C));      /* ends exactly AT `to` */
    push(f, wait_rec(950, 12, 100, EV_A));     /* still open at `to` */

    /* fx_nowait — markers and on-CPU only. The index must REFUSE, and the
     * oracle must carry no signal, so the refusal is not hiding one. */
    FX_NOWAIT = g_nfx;
    f = add_fx("fx_nowait");
    push(f, marker_rec(200, 10, 0xFFFFFFF2U));
    push(f, cpu_rec(300, 11, 10));
    push(f, marker_rec(400, 10, 0xFFFFFFF3U));

    /* fx_one — a single wait: the smallest non-empty answer. */
    FX_ONE = g_nfx;
    f = add_fx("fx_one");
    push(f, wait_rec(500, 7, 100, EV_A));

    /* fx_twoburst — two bursts, two events, two buckets, so num_bursts and
     * bursts_total are both > 1 and the per-bucket burst choice is live. */
    FX_TWOBURST = g_nfx;
    f = add_fx("fx_twoburst");
    push(f, wait_rec(310, 60, 10, EV_C));      /* onsets 300..304: 5 pids */
    push(f, wait_rec(315, 61, 14, EV_C));
    push(f, wait_rec(320, 62, 18, EV_C));
    push(f, wait_rec(325, 63, 22, EV_C));
    push(f, wait_rec(330, 64, 26, EV_C));
    push(f, wait_rec(660, 70, 10, EV_B));      /* onsets 650..655: 4 pids */
    push(f, wait_rec(670, 71, 18, EV_B));
    push(f, wait_rec(680, 72, 26, EV_B));
    push(f, wait_rec(690, 73, 35, EV_B));

    /* fx_pidrepeat — three pids but four records in ONE bucket, one pid
     * twice. Sets union: the peak is 3, never 4. */
    FX_PIDREPEAT = g_nfx;
    f = add_fx("fx_pidrepeat");
    push(f, wait_rec(520, 80, 5, EV_A));
    push(f, wait_rec(530, 81, 5, EV_A));
    push(f, wait_rec(540, 80, 5, EV_A));       /* pid 80 again, same bucket */
    push(f, wait_rec(550, 82, 5, EV_A));

    /* fx_long — one 700 ms wait whose END lands in the last block while its
     * START is back in the first. The clipped-start detector. */
    /* fx_edge — a 4-pid burst, TWO of whose members' end records land
     * exactly ON the window edges (one at `from`, one at `to`). Selection is
     * inclusive at both ends, so all four are in and the burst exists; drop
     * either edge record (a half-open selection at either end) and the group
     * falls to 3 pids and the burst DISAPPEARS. Without this fixture a
     * half-open bound is invisible, because an interval ending exactly at
     * `from` contributes to no bucket and so cannot be caught by peaks. */
    FX_EDGE = g_nfx;
    f = add_fx("fx_edge");
    push(f, wait_rec(100, 50, 10, EV_C));      /* ends AT `from`, onset 90 */
    push(f, wait_rec(200, 51, 108, EV_C));     /* onset 92 */
    push(f, wait_rec(300, 52, 206, EV_C));     /* onset 94 */
    push(f, wait_rec(900, 53, 804, EV_C));     /* ends AT `to`, onset 96 */

    /* fx_cover — a 4-pid burst whose onsets ALL lie before the indexed
     * coverage starts (35..41 ms vs coverage opening at 50 ms), with their
     * end records spread across the stream so a cut puts them in later
     * blocks.
     *
     * Why this fixture has to exist: coverage containment guarantees
     * `cover_from_ns <= from_ns`, so clipping start_ns to the COVERAGE start
     * can never change a bucket assignment (b_lo is 0 for any start <=
     * from). It changes only the burst ONSET and the 10 ms grouping — so
     * without a burst anchored before the coverage, a coverage-clipping bug
     * is invisible. It was: mutation M02 stayed green until this fixture
     * was added. */
    FX_COVER = g_nfx;
    f = add_fx("fx_cover");
    push(f, wait_rec(200, 100, 165, EV_B));    /* onset 35 ms */
    push(f, wait_rec(300, 101, 263, EV_B));    /* onset 37 ms */
    push(f, wait_rec(400, 102, 361, EV_B));    /* onset 39 ms */
    push(f, wait_rec(500, 103, 459, EV_B));    /* onset 41 ms */

    FX_LONG = g_nfx;
    f = add_fx("fx_long");
    push(f, wait_rec(400, 91, 10, EV_A));
    push(f, wait_rec(870, 92, 5, EV_A));
    push(f, wait_rec(880, 90, 700, EV_A));     /* start 180 ms */
}

/* ── index building ───────────────────────────────────────── */

/* `mask` bit i set = cut AFTER record i. Blocks are contiguous in time:
 * block 0 opens at COVER_FROM, the last closes at COVER_TO, and a cut at
 * record i puts the boundary exactly on that record's timestamp — so every
 * record lies inside its own block's bounds and block bounds never go
 * backwards, which is what add_block requires. */
static int build_index(const struct fixture *fx, unsigned mask,
                       int trailing_empty_block,
                       struct pgwt_interval_index *idx)
{
    pgwt_interval_index_init(idx);
    if (g_force_one_block) {
        /* The falsification: ONE block, and no trailing empty one either —
         * otherwise the "more than one block" and "the prefilter skipped a
         * block" ledger rows would stay green on a feed that never splits
         * anything. */
        mask = 0;
        trailing_empty_block = 0;
    }

    uint64_t data_last = trailing_empty_block ? COVER_TO - MS : COVER_TO;
    int bidx = 0, start = 0;
    uint64_t prev_last = COVER_FROM;

    if (fx->n == 0) {
        if (pgwt_interval_index_add_block(idx, bidx++, COVER_FROM, data_last,
                                          NULL, 0) < 0)
            return -1;
    }
    for (int i = 0; i < fx->n; i++) {
        int cut = (i == fx->n - 1) ||
                  (i < 31 && (mask & (1u << i)) != 0);
        if (!cut)
            continue;
        uint64_t first_ns = (bidx == 0) ? COVER_FROM : prev_last;
        uint64_t last_ns = (i == fx->n - 1) ? data_last : fx->ev[i].timestamp_ns;
        if (pgwt_interval_index_add_block(idx, bidx++, first_ns, last_ns,
                                          &fx->ev[start], i - start + 1) < 0)
            return -1;
        prev_last = last_ns;
        start = i + 1;
    }
    if (trailing_empty_block &&
        pgwt_interval_index_add_block(idx, bidx++, data_last, COVER_TO,
                                      NULL, 0) < 0)
        return -1;
    return pgwt_interval_index_seal(idx);
}

/* The raw path: the shipping computation over the FULL record array, which
 * is what src/server.c's handle_concurrency() does. */
static void oracle(const struct fixture *fx, const struct pgwt_filter *f,
                   uint64_t from_ns, uint64_t to_ns, int nb,
                   struct pgwt_concurrency_result *out)
{
    pgwt_compute_concurrency(fx->ev, fx->n, f, from_ns, to_ns, nb,
                             BURST_WIN, BURST_THRESH, out);
}

static void free_conc(struct pgwt_concurrency_result *r)
{
    free(r->peak_sessions);
    free(r->peak_event);
    free(r->bursts);
    memset(r, 0, sizeof(*r));
}

/* ── the comparator ───────────────────────────────────────── */

/* Bit-exact, field by field, including the whole 64-slot pid display sample
 * (both sides are zero-initialised, so the unused tail is comparable too).
 * No epsilon exists here because nothing compared is floating point. */
static int cmp_conc(const struct pgwt_concurrency_result *a,
                    const struct pgwt_concurrency_result *b,
                    char *why, size_t wn)
{
    if (a->failed != b->failed) {
        snprintf(why, wn, "failed %d vs %d", a->failed, b->failed);
        return -1;
    }
    if (a->num_buckets != b->num_buckets) {
        snprintf(why, wn, "num_buckets %d vs %d",
                 a->num_buckets, b->num_buckets);
        return -1;
    }
    if (a->bucket_ns != b->bucket_ns) {
        snprintf(why, wn, "bucket_ns %llu vs %llu",
                 (unsigned long long)a->bucket_ns,
                 (unsigned long long)b->bucket_ns);
        return -1;
    }
    for (int i = 0; i < a->num_buckets; i++) {
        if (a->peak_sessions[i] != b->peak_sessions[i]) {
            snprintf(why, wn, "peak_sessions[%d] %d vs %d", i,
                     a->peak_sessions[i], b->peak_sessions[i]);
            return -1;
        }
        if (a->peak_event[i] != b->peak_event[i]) {
            snprintf(why, wn, "peak_event[%d] %u vs %u", i,
                     a->peak_event[i], b->peak_event[i]);
            return -1;
        }
    }
    if (a->bursts_total != b->bursts_total) {
        snprintf(why, wn, "bursts_total %d vs %d",
                 a->bursts_total, b->bursts_total);
        return -1;
    }
    if (a->num_bursts != b->num_bursts) {
        snprintf(why, wn, "num_bursts %d vs %d",
                 a->num_bursts, b->num_bursts);
        return -1;
    }
    for (int i = 0; i < a->num_bursts; i++) {
        const struct pgwt_burst *x = &a->bursts[i], *y = &b->bursts[i];
        if (x->timestamp_ns != y->timestamp_ns) {
            snprintf(why, wn, "bursts[%d].timestamp_ns %llu vs %llu", i,
                     (unsigned long long)x->timestamp_ns,
                     (unsigned long long)y->timestamp_ns);
            return -1;
        }
        if (x->event_id != y->event_id) {
            snprintf(why, wn, "bursts[%d].event_id %u vs %u", i,
                     x->event_id, y->event_id);
            return -1;
        }
        if (memcmp(x->event_name, y->event_name, sizeof(x->event_name)) != 0) {
            snprintf(why, wn, "bursts[%d].event_name '%.40s' vs '%.40s'", i,
                     x->event_name, y->event_name);
            return -1;
        }
        if (x->num_sessions != y->num_sessions) {
            snprintf(why, wn, "bursts[%d].num_sessions %d vs %d", i,
                     x->num_sessions, y->num_sessions);
            return -1;
        }
        if (x->num_pids != y->num_pids) {
            snprintf(why, wn, "bursts[%d].num_pids %d vs %d", i,
                     x->num_pids, y->num_pids);
            return -1;
        }
        if (memcmp(x->pids, y->pids, sizeof(x->pids)) != 0) {
            snprintf(why, wn, "bursts[%d].pids differ", i);
            return -1;
        }
    }
    return 0;
}

/* ── ledger counters ──────────────────────────────────────── */

static long c_triples, c_cut_masks, c_multiblock, c_cross_block;
static long c_before_cover, c_open_past_to, c_end_before_from;
static long c_peak_gt1, c_bursts, c_burst_multiblock, c_pid_two_chunks;
static long c_no_intervals, c_empty_chunk_skipped, c_rows;
static long c_mismatch;
static char g_first_mismatch[256];

/* LEDGER ONLY — not an oracle, never used to decide correctness. Returns the
 * bucket range a clipped interval touches, using the same arithmetic
 * pgwt_compute_concurrency uses, purely so the ledger can say "these two
 * records really did land in one bucket". */
static void ledger_bucket_range(uint64_t start_ns, uint64_t end_ns,
                                uint64_t from_ns, uint64_t bucket_ns, int nb,
                                int *lo, int *hi)
{
    *lo = 0; *hi = -1;
    if (end_ns <= from_ns)
        return;
    uint64_t into = end_ns - from_ns;
    int b_hi = (int)((into - 1) / bucket_ns);
    if (b_hi > nb - 1) b_hi = nb - 1;
    int b_lo = 0;
    if (start_ns > from_ns) {
        uint64_t q = (start_ns - from_ns) / bucket_ns;
        b_lo = q > (uint64_t)nb ? nb : (int)q;
    }
    *lo = b_lo; *hi = b_hi;
}

/* Which chunk holds row i. */
static int chunk_of_row(const struct pgwt_interval_index *idx, int row)
{
    for (int c = 0; c < idx->n_chunks; c++)
        if (row >= idx->chunks[c].first_row &&
            row < idx->chunks[c].first_row + idx->chunks[c].n_rows)
            return c;
    return -1;
}

/* Ledger: did any returned burst draw its members from two or more BLOCKS?
 * That is silent-wrong mode 2's territory — a boundary interval counted in
 * both its own block's list and an edge decode would push such a burst over
 * the threshold on one pid. */
static int burst_spans_blocks(const struct pgwt_interval_index *idx,
                              const struct pgwt_burst *b)
{
    int seen[64], ns = 0;
    for (int i = 0; i < idx->n_rows; i++) {
        const struct pgwt_wait_interval *iv = &idx->rows[i];
        if (iv->event_id != b->event_id)
            continue;
        if (iv->start_ns < b->timestamp_ns ||
            iv->start_ns > b->timestamp_ns + BURST_WIN)
            continue;
        int c = chunk_of_row(idx, i), dup = 0;
        for (int k = 0; k < ns; k++)
            if (seen[k] == c) { dup = 1; break; }
        if (!dup && ns < 64) seen[ns++] = c;
    }
    return ns >= 2;
}

/* Ledger: same (pid, event) selected from two different BLOCKS with bucket
 * ranges that intersect — the "sets union, they do not add" case. */
static int pid_in_two_chunks_same_bucket(const struct pgwt_interval_index *idx,
                                         uint64_t from_ns, uint64_t to_ns,
                                         uint64_t bucket_ns, int nb)
{
    for (int i = 0; i < idx->n_rows; i++) {
        const struct pgwt_wait_interval *a = &idx->rows[i];
        if (a->end_ns < from_ns || a->end_ns > to_ns) continue;
        int alo, ahi;
        ledger_bucket_range(a->start_ns, a->end_ns, from_ns, bucket_ns, nb,
                            &alo, &ahi);
        if (ahi < alo) continue;
        for (int j = i + 1; j < idx->n_rows; j++) {
            const struct pgwt_wait_interval *b = &idx->rows[j];
            if (b->end_ns < from_ns || b->end_ns > to_ns) continue;
            if (a->pid != b->pid || a->event_id != b->event_id) continue;
            if (chunk_of_row(idx, i) == chunk_of_row(idx, j)) continue;
            int blo, bhi;
            ledger_bucket_range(b->start_ns, b->end_ns, from_ns, bucket_ns,
                                nb, &blo, &bhi);
            if (bhi < blo) continue;
            if (alo <= bhi && blo <= ahi)
                return 1;
        }
    }
    return 0;
}

/* ── literal expectations ─────────────────────────────────── */

static void literal_expectations(void)
{
    printf("--- literal expectations: every boundary class, by value ---\n");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_interval_index idx;
    struct pgwt_interval_index_query_result q;

    /* fx_long, cut so the 700 ms wait's END record is alone in the LAST
     * block while its start is back in the first. Clipping start_ns to that
     * block would leave buckets 0..6 empty. */
    const struct fixture *fl = &g_fx[FX_LONG];
    CHECK(build_index(fl, 0x3u, 0, &idx) == 0,
          "fx_long: index built with cuts after records 0 and 1 (3 blocks)");
    CHECK(idx.n_chunks == 3, "fx_long: 3 blocks fed, got %d", idx.n_chunks);
    CHECK(idx.n_cross_block == 1,
          "fx_long: exactly 1 interval starts before its own block "
          "(got %ld) — the clipped-start detector is live", idx.n_cross_block);
    CHECK(pgwt_interval_index_query(&idx, &f, T(100), T(900), 8,
                                    BURST_WIN, BURST_THRESH, &q) == 0,
          "fx_long: query answered (%s)",
          pgwt_interval_index_refusal_str(q.refused));
    if (q.have_result) {
        /* pid 90 spans [180, 880] -> every one of the 8 buckets of
         * [100, 900). pid 91 adds a second pid in bucket 2 (ends 400 ms),
         * pid 92 a second in bucket 7 (ends 870 ms). */
        int want[8] = {1, 1, 2, 1, 1, 1, 1, 2};
        int ok = (q.result.num_buckets == 8);
        for (int i = 0; ok && i < 8; i++)
            ok = (q.result.peak_sessions[i] == want[i]);
        CHECK(ok, "fx_long: peak_sessions == {1,1,2,1,1,1,1,2}; got "
              "{%d,%d,%d,%d,%d,%d,%d,%d}",
              q.result.peak_sessions[0], q.result.peak_sessions[1],
              q.result.peak_sessions[2], q.result.peak_sessions[3],
              q.result.peak_sessions[4], q.result.peak_sessions[5],
              q.result.peak_sessions[6], q.result.peak_sessions[7]);
        int all_a = 1;
        for (int i = 0; i < 8; i++)
            if (q.result.peak_event[i] != EV_A) all_a = 0;
        CHECK(all_a, "fx_long: every bucket's peak_event is EV_A");
    }
    pgwt_interval_index_query_free(&q);
    pgwt_interval_index_free(&idx);

    /* fx_pidrepeat: 4 records, 3 distinct pids, all in bucket 4. Cut between
     * every pair so pid 80's two records live in different blocks. */
    const struct fixture *fp = &g_fx[FX_PIDREPEAT];
    CHECK(build_index(fp, 0x7u, 0, &idx) == 0,
          "fx_pidrepeat: index built with every record in its own block");
    CHECK(pgwt_interval_index_query(&idx, &f, T(100), T(900), 8,
                                    BURST_WIN, BURST_THRESH, &q) == 0,
          "fx_pidrepeat: query answered");
    if (q.have_result)
        CHECK(q.result.peak_sessions[4] == 3,
              "fx_pidrepeat: bucket 4 peak is 3 DISTINCT pids, not 4 records "
              "(got %d) — sets union, they do not add",
              q.result.peak_sessions[4]);
    pgwt_interval_index_query_free(&q);
    pgwt_interval_index_free(&idx);

    /* fx_main: the burst's four END records cut into four blocks; the wait
     * open past `to`; the wait ending exactly at `to`. */
    const struct fixture *fm = &g_fx[FX_MAIN];
    unsigned burst_cuts = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11);
    CHECK(build_index(fm, burst_cuts, 1, &idx) == 0,
          "fx_main: index built with the burst's 4 ends in 4 blocks "
          "(+ a trailing EMPTY block)");
    CHECK(pgwt_interval_index_query(&idx, &f, T(100), T(900), 8,
                                    BURST_WIN, BURST_THRESH, &q) == 0,
          "fx_main: query answered (%s)",
          pgwt_interval_index_refusal_str(q.refused));
    if (q.have_result) {
        CHECK(q.result.bursts_total == 1,
              "fx_main: exactly 1 burst onset even though its four members "
              "are in four different blocks (got %d)", q.result.bursts_total);
        CHECK(q.result.num_bursts == 1, "fx_main: 1 bucket had a burst (got %d)",
              q.result.num_bursts);
        if (q.result.num_bursts == 1) {
            const struct pgwt_burst *b = &q.result.bursts[0];
            CHECK(b->num_sessions == 4 && b->num_pids == 4,
                  "fx_main: burst has 4 sessions / 4 listed pids (got %d/%d)",
                  b->num_sessions, b->num_pids);
            int pids_ok = (b->pids[0] == 20 && b->pids[1] == 21 &&
                           b->pids[2] == 22 && b->pids[3] == 23);
            CHECK(pids_ok, "fx_main: burst pid list is {20,21,22,23}; got "
                  "{%u,%u,%u,%u}", b->pids[0], b->pids[1], b->pids[2],
                  b->pids[3]);
            CHECK(b->timestamp_ns == T(500),
                  "fx_main: burst onset is the UNCLIPPED first start (500 ms); "
                  "got %+lld ms",
                  (long long)((int64_t)(b->timestamp_ns - BASE) / 1000000));
            CHECK(b->event_id == EV_A, "fx_main: burst event is EV_A");
        }
        CHECK(q.excluded_open_past_to == 1,
              "fx_main: exactly 1 interval excluded for ending past `to` "
              "(got %ld) — the raw path's exclusion, reproduced not 'fixed'",
              q.excluded_open_past_to);
        /* The wait ending exactly AT `to` (pid 40, 850..900) is INSIDE the
         * window: buckets 7 only. The one ending exactly at `from` (pid 41)
         * contributes to no bucket. */
        CHECK(q.result.peak_sessions[7] >= 1,
              "fx_main: the wait ending exactly at `to` is selected "
              "(bucket 7 peak %d >= 1)", q.result.peak_sessions[7]);
        CHECK(q.intervals_used == 9,
              "fx_main: 9 of its 14 records are indexable waits inside the "
              "window (got %ld): the marker, the on-CPU gap, the idle wait, "
              "the impossible record and the one open past `to` are all out",
              q.intervals_used);
    }
    CHECK(idx.n_records_skipped == 4,
          "fx_main: the shared predicate skipped exactly 4 records "
          "(marker, on-CPU, idle, impossible); got %ld",
          idx.n_records_skipped);
    pgwt_interval_index_query_free(&q);
    pgwt_interval_index_free(&idx);

    /* fx_edge: selection is INCLUSIVE at both window ends. A half-open bound
     * at either end removes one burst member and the burst vanishes — which
     * peaks alone cannot see, because the member ending at `from`
     * contributes to no bucket at all. */
    const struct fixture *fe = &g_fx[FX_EDGE];
    CHECK(build_index(fe, 0x7u, 0, &idx) == 0,
          "fx_edge: index built with each record in its own block");
    CHECK(pgwt_interval_index_query(&idx, &f, T(100), T(900), 8,
                                    BURST_WIN, BURST_THRESH, &q) == 0,
          "fx_edge: query answered");
    if (q.have_result) {
        CHECK(q.intervals_used == 4,
              "fx_edge: all 4 records selected, including the one ending "
              "exactly at `from` and the one ending exactly at `to` (got %ld)",
              q.intervals_used);
        CHECK(q.result.bursts_total == 1 && q.result.num_bursts == 1,
              "fx_edge: the 4-pid burst survives (bursts_total %d, "
              "num_bursts %d)", q.result.bursts_total, q.result.num_bursts);
        if (q.result.num_bursts == 1) {
            const struct pgwt_burst *b = &q.result.bursts[0];
            CHECK(b->num_sessions == 4 && b->pids[0] == 50 &&
                  b->pids[1] == 51 && b->pids[2] == 52 && b->pids[3] == 53,
                  "fx_edge: burst pid list is {50,51,52,53} (got %d sessions, "
                  "{%u,%u,%u,%u})", b->num_sessions, b->pids[0], b->pids[1],
                  b->pids[2], b->pids[3]);
            CHECK(b->timestamp_ns == T(90),
                  "fx_edge: onset is 90 ms — BEFORE the window opens, "
                  "unclipped");
        }
        CHECK(q.result.peak_sessions[0] == 3,
              "fx_edge: bucket 0 peak is 3 (the member ending exactly at "
              "`from` contributes to no bucket); got %d",
              q.result.peak_sessions[0]);
    }
    pgwt_interval_index_query_free(&q);
    pgwt_interval_index_free(&idx);

    /* fx_cover: a burst anchored BEFORE the indexed coverage opens. The only
     * observable effect of a start clipped to cover_from_ns is on the burst
     * onset and its 10 ms grouping, so this is the one shape that sees it. */
    const struct fixture *fc = &g_fx[FX_COVER];
    CHECK(build_index(fc, 0x7u, 0, &idx) == 0,
          "fx_cover: index built with each record in its own block");
    CHECK(idx.n_start_before_cover == 4,
          "fx_cover: all 4 intervals start before the indexed coverage "
          "(got %ld)", idx.n_start_before_cover);
    CHECK(pgwt_interval_index_query(&idx, &f, T(100), T(900), 8,
                                    BURST_WIN, BURST_THRESH, &q) == 0,
          "fx_cover: query answered");
    if (q.have_result) {
        CHECK(q.result.bursts_total == 1 && q.result.num_bursts == 1,
              "fx_cover: the burst survives (bursts_total %d, num_bursts %d)",
              q.result.bursts_total, q.result.num_bursts);
        if (q.result.num_bursts == 1) {
            const struct pgwt_burst *b = &q.result.bursts[0];
            CHECK(b->timestamp_ns == T(35),
                  "fx_cover: onset is 35 ms — before the coverage start "
                  "(50 ms) and before the window (100 ms), UNCLIPPED");
            CHECK(b->num_sessions == 4 && b->pids[0] == 100 &&
                  b->pids[1] == 101 && b->pids[2] == 102 &&
                  b->pids[3] == 103,
                  "fx_cover: burst pid list is {100,101,102,103} (got %d "
                  "sessions, {%u,%u,%u,%u})", b->num_sessions, b->pids[0],
                  b->pids[1], b->pids[2], b->pids[3]);
        }
    }
    pgwt_interval_index_query_free(&q);
    pgwt_interval_index_free(&idx);
}

/* ── pre-windowing equivalence ────────────────────────────── */

/* The oracle above is handed the WHOLE fixture array, while the real server
 * hands pgwt_compute_concurrency only the records the loader selected for
 * the window. Those two must agree, or the oracle is not the product's
 * computation. Checked rather than assumed. */
static void prewindow_equivalence(void)
{
    printf("--- the oracle's input: full array == loader-windowed array ---\n");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    int cases = 0, bad = 0;
    char why[256] = "";
    const int wins[][2] = {{100, 900}, {50, 1200}, {240, 520}, {500, 901}};

    for (int x = 0; x < g_nfx; x++) {
        const struct fixture *fx = &g_fx[x];
        for (int w = 0; w < 4; w++) {
            uint64_t from = T(wins[w][0]), to = T(wins[w][1]);
            /* src/server.c's record predicate: skip timestamp_ns < from_m
             * or > to_m. Inclusive at both ends, keyed on the END. */
            struct fixture cut;
            memset(&cut, 0, sizeof(cut));
            cut.name = fx->name;
            for (int i = 0; i < fx->n; i++)
                if (fx->ev[i].timestamp_ns >= from &&
                    fx->ev[i].timestamp_ns <= to)
                    cut.ev[cut.n++] = fx->ev[i];
            if (cut.n == 0)
                continue;   /* count==0 is the documented seam; see the header */
            for (int nb = 8; nb <= 23; nb += 15) {
                struct pgwt_concurrency_result a, b;
                oracle(fx, &f, from, to, nb, &a);
                oracle(&cut, &f, from, to, nb, &b);
                cases++;
                if (cmp_conc(&a, &b, why, sizeof(why)) != 0) {
                    bad++;
                    printf("    %s w=[%d,%d] nb=%d: %s\n", fx->name,
                           wins[w][0], wins[w][1], nb, why);
                }
                free_conc(&a);
                free_conc(&b);
            }
        }
    }
    CHECK(cases > 20, "%d (fixture, window, resolution) cases compared",
          cases);
    CHECK(bad == 0, "the full array and the loader-windowed array agree "
          "everywhere (%d disagreements)", bad);
}

/* ── the sweep ────────────────────────────────────────────── */

static const int g_wins[][2] = {
    {100, 900}, {50, 1200}, {240, 520}, {500, 901}, {899, 901}, {850, 960},
};
#define NWINS ((int)(sizeof(g_wins) / sizeof(g_wins[0])))
static const int g_res[2] = {8, 23};

static void sweep_cuts(void)
{
    printf("--- sweep: every block cut x every window x two resolutions ---\n");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    char why[256] = "";

    for (int x = 0; x < g_nfx; x++) {
        const struct fixture *fx = &g_fx[x];
        int ncut = fx->n > 1 ? fx->n - 1 : 0;
        unsigned total = ncut >= 31 ? 0x7FFFFFFFu : (1u << ncut);
        /* Enumerate every cut set when there are few; otherwise stride-sample
         * and ALWAYS include no-cut and all-cuts. */
        unsigned step = total > 512 ? total / 512 : 1;
        for (unsigned m = 0; ; m += step) {
            unsigned mask = m < total ? m : (total - 1);
            struct pgwt_interval_index idx;
            if (build_index(fx, mask, (mask & 1u) ? 1 : 0, &idx) != 0) {
                CHECK(0, "%s mask=0x%x: index build or seal FAILED (%s)",
                      fx->name, mask,
                      pgwt_interval_index_refusal_str(idx.build_refusal));
                pgwt_interval_index_free(&idx);
                break;
            }
            c_cut_masks++;
            if (idx.n_chunks > 1) c_multiblock++;
            c_cross_block += idx.n_cross_block;
            c_before_cover += idx.n_start_before_cover;

            for (int w = 0; w < NWINS; w++) {
                uint64_t from = T(g_wins[w][0]), to = T(g_wins[w][1]);
                for (int r = 0; r < 2; r++) {
                    int nb = g_res[r];
                    struct pgwt_concurrency_result ref;
                    oracle(fx, &f, from, to, nb, &ref);

                    struct pgwt_interval_index_query_result q;
                    int rc = pgwt_interval_index_query(&idx, &f, from, to, nb,
                                                       BURST_WIN,
                                                       BURST_THRESH, &q);
                    if (rc != 0) {
                        /* The only refusal the sweep's windows can produce
                         * is "nothing selected" — and then the raw answer
                         * must carry NO signal, or the refusal is hiding
                         * one. */
                        if (q.refused ==
                            PGWT_INTERVAL_INDEX_REFUSE_NO_INTERVALS) {
                            int sig = ref.bursts_total;
                            for (int i = 0; i < ref.num_buckets; i++)
                                sig += ref.peak_sessions[i];
                            if (sig != 0) {
                                c_mismatch++;
                                if (!g_first_mismatch[0])
                                    snprintf(g_first_mismatch,
                                             sizeof(g_first_mismatch),
                                             "%s mask=0x%x w=[%d,%d] nb=%d: "
                                             "refused no-intervals but raw "
                                             "has signal %d", fx->name, mask,
                                             g_wins[w][0], g_wins[w][1], nb,
                                             sig);
                            }
                            c_no_intervals++;
                        } else {
                            c_mismatch++;
                            if (!g_first_mismatch[0])
                                snprintf(g_first_mismatch,
                                         sizeof(g_first_mismatch),
                                         "%s mask=0x%x w=[%d,%d] nb=%d: "
                                         "unexpected refusal %s", fx->name,
                                         mask, g_wins[w][0], g_wins[w][1], nb,
                                         pgwt_interval_index_refusal_str(
                                             q.refused));
                        }
                        free_conc(&ref);
                        pgwt_interval_index_query_free(&q);
                        continue;
                    }

                    c_triples++;
                    c_rows += q.intervals_used;
                    c_open_past_to += q.excluded_open_past_to;
                    c_end_before_from += q.excluded_end_before_from;
                    c_empty_chunk_skipped += q.chunks_total - q.chunks_scanned;
                    c_bursts += q.result.bursts_total;
                    for (int i = 0; i < q.result.num_buckets; i++)
                        if (q.result.peak_sessions[i] > 1) { c_peak_gt1++; break; }
                    for (int i = 0; i < q.result.num_bursts; i++)
                        if (burst_spans_blocks(&idx, &q.result.bursts[i])) {
                            c_burst_multiblock++;
                            break;
                        }
                    if (pid_in_two_chunks_same_bucket(&idx, from, to,
                                                      q.result.bucket_ns, nb))
                        c_pid_two_chunks++;

                    if (cmp_conc(&ref, &q.result, why, sizeof(why)) != 0) {
                        c_mismatch++;
                        if (!g_first_mismatch[0])
                            snprintf(g_first_mismatch, sizeof(g_first_mismatch),
                                     "%s mask=0x%x w=[%d,%d] nb=%d: %s",
                                     fx->name, mask, g_wins[w][0],
                                     g_wins[w][1], nb, why);
                    }
                    free_conc(&ref);
                    pgwt_interval_index_query_free(&q);
                }
            }
            pgwt_interval_index_free(&idx);
            if (m >= total - 1 || total == 0)
                break;
        }
    }
    CHECK(c_mismatch == 0, "no disagreement in %ld comparisons%s%s",
          c_triples, c_mismatch ? " — first: " : "",
          c_mismatch ? g_first_mismatch : "");
}

/* ── comparator probes ────────────────────────────────────── */

static void comparator_probes(void)
{
    printf("--- comparator probes: the smallest possible difference ---\n");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    struct pgwt_concurrency_result base;
    oracle(&g_fx[FX_TWOBURST], &f, T(100), T(900), 8, &base);
    CHECK(base.num_buckets == 8 && base.num_bursts == 2 &&
          base.bursts_total == 2,
          "probe base: 8 buckets, 2 bursts, bursts_total 2 (got %d/%d/%d)",
          base.num_buckets, base.num_bursts, base.bursts_total);

    char why[256];
#define PROBE(label, mutate) do {                                        \
    struct pgwt_concurrency_result m = base;                             \
    int *ps = malloc(sizeof(int) * (size_t)base.num_buckets);            \
    uint32_t *pe = malloc(sizeof(uint32_t) * (size_t)base.num_buckets);  \
    struct pgwt_burst *bs = malloc(sizeof(*bs) * (size_t)base.num_bursts);\
    memcpy(ps, base.peak_sessions, sizeof(int) * (size_t)base.num_buckets); \
    memcpy(pe, base.peak_event, sizeof(uint32_t) * (size_t)base.num_buckets); \
    memcpy(bs, base.bursts, sizeof(*bs) * (size_t)base.num_bursts);      \
    m.peak_sessions = ps; m.peak_event = pe; m.bursts = bs;              \
    { mutate; }                                                          \
    why[0] = '\0';                                                       \
    int differ = cmp_conc(&base, &m, why, sizeof(why));                  \
    CHECK(differ != 0, "probe %s: comparator reports it (%s)", label, why); \
    free(ps); free(pe); free(bs);                                        \
} while (0)

    PROBE("num_buckets-1", m.num_buckets = base.num_buckets - 1);
    PROBE("bucket_ns+1", m.bucket_ns = base.bucket_ns + 1);
    PROBE("peak_sessions[3]+1", ps[3] += 1);
    PROBE("peak_sessions[0]+1", ps[0] += 1);
    PROBE("peak_event[3]+1", pe[3] += 1);
    PROBE("bursts_total+1", m.bursts_total = base.bursts_total + 1);
    PROBE("num_bursts-1", m.num_bursts = base.num_bursts - 1);
    PROBE("burst[0].timestamp_ns+1", bs[0].timestamp_ns += 1);
    PROBE("burst[0].event_id+1", bs[0].event_id += 1);
    PROBE("burst[0].event_name", bs[0].event_name[0] ^= 0x20);
    PROBE("burst[0].num_sessions+1", bs[0].num_sessions += 1);
    PROBE("burst[0].num_pids-1", bs[0].num_pids -= 1);
    PROBE("burst[0].pids[0]+1", bs[0].pids[0] += 1);
    PROBE("burst[1].pids[2]+1", bs[1].pids[2] += 1);
    PROBE("burst[0].pids[63] (unused tail)", bs[0].pids[63] = 777);
    PROBE("failed flag", m.failed = 1);
#undef PROBE
    free_conc(&base);
}

/* ── bypass suite ─────────────────────────────────────────── */

/* Every way the index can fail to establish an answer must REFUSE with its
 * own reason and leave have_result == 0. A gate that cannot see must refuse,
 * never approve. */
static void expect_refusal(const char *label,
                           struct pgwt_interval_index *idx,
                           const struct pgwt_filter *f,
                           uint64_t from, uint64_t to, int nb,
                           enum pgwt_interval_index_refusal want)
{
    struct pgwt_interval_index_query_result q;
    int rc = pgwt_interval_index_query(idx, f, from, to, nb, BURST_WIN,
                                       BURST_THRESH, &q);
    CHECK(rc == -1 && q.refused == want && q.have_result == 0 &&
          q.result.num_buckets == 0 && q.result.peak_sessions == NULL &&
          q.result.bursts == NULL,
          "%s: REFUSES with %s and no result (rc=%d got=%s have=%d)",
          label, pgwt_interval_index_refusal_str(want), rc,
          pgwt_interval_index_refusal_str(q.refused), q.have_result);
    pgwt_interval_index_query_free(&q);
}

static void bypass_suite(void)
{
    printf("--- bypass suite: a gate that cannot see must REFUSE ---\n");
    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));
    const struct fixture *fm = &g_fx[FX_MAIN];
    struct pgwt_interval_index idx;

    /* NULL arguments. */
    expect_refusal("NULL index", NULL, &f, T(100), T(900), 8,
                   PGWT_INTERVAL_INDEX_REFUSE_NULL);
    CHECK(build_index(fm, 0x5u, 0, &idx) == 0, "bypass: base index built");
    expect_refusal("NULL filter", &idx, NULL, T(100), T(900), 8,
                   PGWT_INTERVAL_INDEX_REFUSE_NULL);

    /* The answerable control: without it every refusal below could be the
     * module refusing unconditionally. */
    {
        struct pgwt_interval_index_query_result q;
        CHECK(pgwt_interval_index_query(&idx, &f, T(100), T(900), 8,
                                        BURST_WIN, BURST_THRESH, &q) == 0 &&
              q.have_result && q.result.num_buckets == 8,
              "control: the same index DOES answer a good request");
        pgwt_interval_index_query_free(&q);
    }

    /* Filtered requests: version 1 indexes the unfiltered set. */
    {
        struct pgwt_filter g;
        memset(&g, 0, sizeof(g)); g.pid = 20;
        expect_refusal("pid filter", &idx, &g, T(100), T(900), 8,
                       PGWT_INTERVAL_INDEX_REFUSE_FILTERED);
        memset(&g, 0, sizeof(g)); g.event_id = EV_A;
        expect_refusal("event filter", &idx, &g, T(100), T(900), 8,
                       PGWT_INTERVAL_INDEX_REFUSE_FILTERED);
        memset(&g, 0, sizeof(g)); snprintf(g.class_name, sizeof(g.class_name), "IO");
        expect_refusal("class filter", &idx, &g, T(100), T(900), 8,
                       PGWT_INTERVAL_INDEX_REFUSE_FILTERED);
        memset(&g, 0, sizeof(g)); g.query_id = 42;
        expect_refusal("query_id filter", &idx, &g, T(100), T(900), 8,
                       PGWT_INTERVAL_INDEX_REFUSE_FILTERED);
    }

    /* Window shapes. */
    expect_refusal("inverted window", &idx, &f, T(900), T(100), 8,
                   PGWT_INTERVAL_INDEX_REFUSE_BAD_WINDOW);
    expect_refusal("zero-width window", &idx, &f, T(500), T(500), 8,
                   PGWT_INTERVAL_INDEX_REFUSE_BAD_WINDOW);
    expect_refusal("num_buckets 0", &idx, &f, T(100), T(900), 0,
                   PGWT_INTERVAL_INDEX_REFUSE_BAD_BUCKETS);
    expect_refusal("num_buckets negative", &idx, &f, T(100), T(900), -3,
                   PGWT_INTERVAL_INDEX_REFUSE_BAD_BUCKETS);
    expect_refusal("window past coverage end", &idx, &f, T(100),
                   COVER_TO + MS, 8, PGWT_INTERVAL_INDEX_REFUSE_RANGE);
    expect_refusal("window before coverage start", &idx, &f, COVER_FROM - MS,
                   T(900), 8, PGWT_INTERVAL_INDEX_REFUSE_RANGE);

    /* Unsealed: a build in progress must not answer. */
    idx.sealed = 0;
    expect_refusal("unsealed", &idx, &f, T(100), T(900), 8,
                   PGWT_INTERVAL_INDEX_REFUSE_UNSEALED);
    CHECK(pgwt_interval_index_seal(&idx) == 0, "re-seal succeeds");

    /* Wrong version stamp. */
    idx.version = PGWT_INTERVAL_INDEX_VERSION + 7;
    expect_refusal("wrong version", &idx, &f, T(100), T(900), 8,
                   PGWT_INTERVAL_INDEX_REFUSE_VERSION);
    idx.version = PGWT_INTERVAL_INDEX_VERSION;

    /* A hand-corrupted row order: seal must catch it rather than let the
     * binary search read a sorted array that is not sorted. */
    if (idx.n_rows >= 2) {
        struct pgwt_wait_interval t = idx.rows[0];
        idx.rows[0] = idx.rows[idx.n_rows - 1];
        idx.rows[idx.n_rows - 1] = t;
        CHECK(pgwt_interval_index_seal(&idx) == -1,
              "seal REFUSES a row array that is not sorted by end_ns");
        expect_refusal("unsorted rows", &idx, &f, T(100), T(900), 8,
                       PGWT_INTERVAL_INDEX_REFUSE_GAP);
    }
    pgwt_interval_index_free(&idx);

    /* Empty index: no block was ever added. NOT "no concurrency". */
    pgwt_interval_index_init(&idx);
    CHECK(pgwt_interval_index_seal(&idx) == 0, "empty index seals");
    expect_refusal("empty index", &idx, &f, T(100), T(900), 8,
                   PGWT_INTERVAL_INDEX_REFUSE_EMPTY);
    pgwt_interval_index_free(&idx);

    /* A partial / reordered block range. */
    pgwt_interval_index_init(&idx);
    CHECK(pgwt_interval_index_add_block(&idx, 0, COVER_FROM, T(500),
                                        fm->ev, 8) == 0,
          "gap: block 0 accepted");
    CHECK(pgwt_interval_index_add_block(&idx, 2, T(500), COVER_TO,
                                        &fm->ev[8], fm->n - 8) == -1,
          "gap: block 2 after block 0 is REFUSED (a skipped block)");
    expect_refusal("skipped block", &idx, &f, T(100), T(400), 8,
                   PGWT_INTERVAL_INDEX_REFUSE_GAP);
    pgwt_interval_index_free(&idx);

    pgwt_interval_index_init(&idx);
    CHECK(pgwt_interval_index_add_block(&idx, 1, COVER_FROM, T(500),
                                        fm->ev, 8) == -1,
          "reorder: a feed starting at block 1 is REFUSED");
    expect_refusal("reordered feed", &idx, &f, T(100), T(400), 8,
                   PGWT_INTERVAL_INDEX_REFUSE_GAP);
    pgwt_interval_index_free(&idx);

    /* A record outside its own block's bounds, and inverted block bounds. */
    pgwt_interval_index_init(&idx);
    CHECK(pgwt_interval_index_add_block(&idx, 0, T(200), T(300),
                                        fm->ev, 8) == -1,
          "record outside [block_first, block_last] is REFUSED");
    pgwt_interval_index_free(&idx);

    pgwt_interval_index_init(&idx);
    CHECK(pgwt_interval_index_add_block(&idx, 0, T(900), T(100),
                                        NULL, 0) == -1,
          "block_first > block_last is REFUSED");
    pgwt_interval_index_free(&idx);

    pgwt_interval_index_init(&idx);
    CHECK(pgwt_interval_index_add_block(&idx, 0, COVER_FROM, T(1000),
                                        fm->ev, fm->n) == 0,
          "overlap: block 0 accepted");
    CHECK(pgwt_interval_index_add_block(&idx, 1, T(500), COVER_TO,
                                        NULL, 0) == -1,
          "a block starting BEFORE the previous one ended is REFUSED");
    pgwt_interval_index_free(&idx);

    /* An out-of-order record inside one block. */
    {
        struct pgwt_trace_event two[2];
        two[0] = wait_rec(500, 1, 10, EV_A);
        two[1] = wait_rec(400, 2, 10, EV_A);
        pgwt_interval_index_init(&idx);
        CHECK(pgwt_interval_index_add_block(&idx, 0, COVER_FROM, COVER_TO,
                                            two, 2) == -1,
              "an out-of-order record is REFUSED, never sorted");
        expect_refusal("out-of-order record", &idx, &f, T(100), T(900), 8,
                       PGWT_INTERVAL_INDEX_REFUSE_GAP);
        pgwt_interval_index_free(&idx);
    }

    /* THE THING BEING CHECKED IS ABSENT, not wrong: a window (and a whole
     * fixture) with no waits at all. The oracle is checked to carry no
     * signal, so this refusal cannot be hiding a real answer. */
    {
        const struct fixture *fn = &g_fx[FX_NOWAIT];
        CHECK(build_index(fn, 0x1u, 0, &idx) == 0,
              "fx_nowait: index built (0 intervals from 3 records)");
        CHECK(idx.n_rows == 0 && idx.n_records_skipped == 3,
              "fx_nowait: 0 intervals stored, 3 records skipped (got %d/%ld)",
              idx.n_rows, idx.n_records_skipped);
        expect_refusal("fixture with no waits", &idx, &f, T(100), T(900), 8,
                       PGWT_INTERVAL_INDEX_REFUSE_NO_INTERVALS);
        struct pgwt_concurrency_result ref;
        oracle(fn, &f, T(100), T(900), 8, &ref);
        int sig = ref.bursts_total;
        for (int i = 0; i < ref.num_buckets; i++) sig += ref.peak_sessions[i];
        CHECK(sig == 0, "fx_nowait: the RAW answer carries no signal either "
              "(%d) — the refusal is not hiding one", sig);
        free_conc(&ref);
        pgwt_interval_index_free(&idx);
    }
    {
        /* A covered window that simply contains no interval. */
        CHECK(build_index(&g_fx[FX_ONE], 0, 0, &idx) == 0,
              "fx_one: index built");
        expect_refusal("covered window with no interval in it", &idx, &f,
                       T(1000), T(1100), 8,
                       PGWT_INTERVAL_INDEX_REFUSE_NO_INTERVALS);
        pgwt_interval_index_free(&idx);
    }

    /* Every injected allocation failure. An unreachable refusal is
     * indistinguishable from one that approves. */
    setenv("PGWT_TEST_ALLOC_FAIL", "interval_index_rows", 1);
    pgwt_interval_index_init(&idx);
    CHECK(pgwt_interval_index_add_block(&idx, 0, COVER_FROM, COVER_TO,
                                        fm->ev, fm->n) == -1,
          "alloc fail at interval_index_rows: build FAILS");
    expect_refusal("build alloc failure (rows)", &idx, &f, T(100), T(900), 8,
                   PGWT_INTERVAL_INDEX_REFUSE_ALLOC);
    pgwt_interval_index_free(&idx);

    setenv("PGWT_TEST_ALLOC_FAIL", "interval_index_chunks", 1);
    pgwt_interval_index_init(&idx);
    CHECK(pgwt_interval_index_add_block(&idx, 0, COVER_FROM, COVER_TO,
                                        fm->ev, fm->n) == -1,
          "alloc fail at interval_index_chunks: build FAILS");
    expect_refusal("build alloc failure (chunks)", &idx, &f, T(100), T(900), 8,
                   PGWT_INTERVAL_INDEX_REFUSE_ALLOC);
    pgwt_interval_index_free(&idx);

    unsetenv("PGWT_TEST_ALLOC_FAIL");
    CHECK(build_index(fm, 0x5u, 0, &idx) == 0, "alloc: index rebuilt clean");
    setenv("PGWT_TEST_ALLOC_FAIL", "interval_index_query_events", 1);
    expect_refusal("query alloc failure", &idx, &f, T(100), T(900), 8,
                   PGWT_INTERVAL_INDEX_REFUSE_ALLOC);
    setenv("PGWT_TEST_ALLOC_FAIL", "concurrency_entries", 1);
    expect_refusal("the shipping computation itself failing", &idx, &f,
                   T(100), T(900), 8, PGWT_INTERVAL_INDEX_REFUSE_COMPUTE);
    unsetenv("PGWT_TEST_ALLOC_FAIL");
    pgwt_interval_index_free(&idx);
}

/* ── the measured claim ───────────────────────────────────── */

static void measure_compaction(void)
{
    printf("--- measured: compaction, not an O(blocks) jump ---\n");
    const int n = 120000;
    struct pgwt_trace_event *ev =
        (struct pgwt_trace_event *)calloc((size_t)n, sizeof(*ev));
    if (!ev) { CHECK(0, "measure: calloc failed"); return; }

    const uint32_t evs[3] = {EV_A, EV_B, EV_C};
    for (int i = 0; i < n; i++) {
        uint64_t ts = BASE + (uint64_t)i * 100000ULL;   /* 0.1 ms apart */
        if (i % 3 == 0) {
            ev[i] = wait_rec(0, (uint32_t)(1 + i % 64), 0, evs[i % 3]);
            ev[i].timestamp_ns = ts;
            ev[i].duration_ns = (uint64_t)(50000 + (i % 97) * 1000);
        } else if (i % 3 == 1) {
            ev[i] = cpu_rec(0, (uint32_t)(1 + i % 64), 0);
            ev[i].timestamp_ns = ts;
            ev[i].duration_ns = 10000;
        } else {
            ev[i] = marker_rec(0, (uint32_t)(1 + i % 64), 0xFFFFFFF2U);
            ev[i].timestamp_ns = ts;
        }
    }
    uint64_t span_to = ev[n - 1].timestamp_ns;

    const int nblocks = 40, per = n / nblocks;
    struct pgwt_interval_index idx;
    pgwt_interval_index_init(&idx);
    uint64_t prev_last = BASE;
    int ok = 1;
    for (int b = 0; b < nblocks; b++) {
        int lo = b * per, hi = (b == nblocks - 1) ? n - 1 : (lo + per - 1);
        uint64_t last = (b == nblocks - 1) ? span_to : ev[hi].timestamp_ns;
        if (pgwt_interval_index_add_block(&idx, b, prev_last, last,
                                          &ev[lo], hi - lo + 1) != 0) {
            ok = 0; break;
        }
        prev_last = last;
    }
    CHECK(ok && pgwt_interval_index_seal(&idx) == 0,
          "measure: %d blocks, %d records, %d intervals indexed",
          nblocks, n, idx.n_rows);

    uint64_t from = BASE + 3000ULL * MS, to = BASE + 9000ULL * MS;
    int in_window = 0;
    for (int i = 0; i < n; i++)
        if (ev[i].timestamp_ns >= from && ev[i].timestamp_ns <= to)
            in_window++;

    struct pgwt_filter f;
    memset(&f, 0, sizeof(f));

    struct timespec t0, t1;
    struct pgwt_interval_index_query_result q;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = pgwt_interval_index_query(&idx, &f, from, to, 60, BURST_WIN,
                                       BURST_THRESH, &q);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double idx_us = (t1.tv_sec - t0.tv_sec) * 1e6 +
                    (t1.tv_nsec - t0.tv_nsec) / 1e3;
    CHECK(rc == 0 && q.have_result, "measure: index answered (%s)",
          pgwt_interval_index_refusal_str(q.refused));

    struct pgwt_concurrency_result ref;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pgwt_compute_concurrency(ev, n, &f, from, to, 60, BURST_WIN,
                             BURST_THRESH, &ref);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double raw_us = (t1.tv_sec - t0.tv_sec) * 1e6 +
                    (t1.tv_nsec - t0.tv_nsec) / 1e3;

    char why[256] = "";
    CHECK(rc == 0 && cmp_conc(&ref, &q.result, why, sizeof(why)) == 0,
          "measure: 120k-record fixture agrees bit-exactly%s%s",
          why[0] ? " — " : "", why);

    /* Deterministic counters, not timings. */
    CHECK(q.intervals_used * 2 < in_window,
          "compaction: %ld intervals materialised for %d records in window",
          q.intervals_used, in_window);
    long raw_bytes = (long)in_window * (long)sizeof(struct pgwt_trace_event);
    long raw_total = (long)n * (long)sizeof(struct pgwt_trace_event);
    long index_bytes = (long)idx.n_rows * (long)sizeof(struct pgwt_wait_interval);
    /* The honest win, stated as the two separate things it is:
     *   STORAGE — 24-byte waits-only rows instead of a 48-byte record for
     *   every event. Here 1 record in 3 is a wait, so ~6x.
     *   MATERIALISED — the transient array handed to the computation. It is
     *   48 bytes per interval (it IS a record array), so its only saving is
     *   the non-wait records that never appear: ~3x.
     * Neither is the O(blocks) collapse Phases 1 and 2 get, and this test
     * asserts neither as if it were. */
    CHECK(index_bytes * 5 < raw_total,
          "storage compaction: %ld index bytes for %ld raw bytes (%.1fx)",
          index_bytes, raw_total,
          raw_total / (double)(index_bytes ? index_bytes : 1));
    CHECK(q.bytes_materialised * 2 < raw_bytes,
          "materialised compaction: %ld bytes vs %ld raw bytes in window "
          "(%.1fx)", q.bytes_materialised, raw_bytes,
          raw_bytes / (double)(q.bytes_materialised ? q.bytes_materialised : 1));
    CHECK(q.chunks_total - q.chunks_scanned > 0,
          "the chunk prefilter SKIPPED %ld of %ld blocks (not a no-op)",
          q.chunks_total - q.chunks_scanned, q.chunks_total);
    CHECK(q.result.bursts_total > 0,
          "measure: the fixture produced %d burst onsets (not a vacuous "
          "comparison)", q.result.bursts_total);

    printf("  MEASURED n=1: records=%d in_window=%d index_rows=%d "
           "index_bytes=%ld raw_bytes_total=%ld intervals_used=%ld "
           "bytes_materialised=%ld raw_bytes_in_window=%ld "
           "blocks=%ld scanned=%ld index_us=%.1f raw_us=%.1f "
           "(timings REPORTED, never asserted)\n",
           n, in_window, idx.n_rows, index_bytes, raw_total, q.intervals_used,
           q.bytes_materialised, raw_bytes, q.chunks_total, q.chunks_scanned,
           idx_us, raw_us);

    free_conc(&ref);
    pgwt_interval_index_query_free(&q);
    pgwt_interval_index_free(&idx);
    free(ev);
}

/* ── the non-vacuity ledger ───────────────────────────────── */

/* Each of these FAILS the test when zero. `./test_interval_index oneblock`
 * feeds every fixture as one block and must redden the boundary rows. */
static void non_vacuity(void)
{
    printf("--- non-vacuity ledger: the interesting cases really ran ---\n");
    CHECK(c_cut_masks > 50, "%ld block-cut sets were built", c_cut_masks);
    CHECK(c_triples > 500, "%ld (cut, window, resolution) answers compared",
          c_triples);
    CHECK(c_rows > 0, "%ld intervals were actually compared (not 0 == 0)",
          c_rows);
    CHECK(c_multiblock > 0, "%ld indexes had MORE THAN ONE block — a "
          "one-block suite never tests the boundary rule", c_multiblock);
    CHECK(c_cross_block > 0, "%ld intervals straddled an INTERNAL block "
          "boundary — stored in a later block, started before it opened "
          "(the clipped-start detector)", c_cross_block);
    CHECK(c_before_cover > 0, "%ld intervals started before the indexed "
          "coverage itself", c_before_cover);
    CHECK(c_open_past_to > 0, "%ld intervals were excluded for ending past "
          "`to` (the raw path's exclusion, exercised)", c_open_past_to);
    CHECK(c_end_before_from > 0, "%ld intervals were excluded for ending "
          "before `from`", c_end_before_from);
    CHECK(c_pid_two_chunks > 0, "%ld answers had the SAME pid selected from "
          "two different blocks inside one bucket", c_pid_two_chunks);
    CHECK(c_peak_gt1 > 0, "%ld answers had a bucket peak above 1", c_peak_gt1);
    CHECK(c_bursts > 0, "%ld burst onsets were detected across the sweep",
          c_bursts);
    CHECK(c_burst_multiblock > 0, "%ld answers contained a burst whose "
          "members live in TWO OR MORE blocks", c_burst_multiblock);
    CHECK(c_no_intervals > 0, "%ld windows selected nothing and REFUSED",
          c_no_intervals);
    CHECK(c_empty_chunk_skipped > 0, "the prefilter skipped a block %ld "
          "times across the sweep", c_empty_chunk_skipped);
}

int main(int argc, char **argv)
{
    pgwt_init_event_names(18);
    g_force_one_block = (argc > 1 && strcmp(argv[1], "oneblock") == 0);

    printf("=== test_interval_index: the concurrency interval index "
           "(Phase 4) ===\n");
    printf("index version under test: %d\n", PGWT_INTERVAL_INDEX_VERSION);
    if (g_force_one_block)
        printf("MODE: oneblock — DELIBERATE FALSIFICATION. Every fixture is "
               "fed as a SINGLE block, so the boundary ledger MUST go red. "
               "A green run here means the ledger cannot see.\n");

    build_fixtures();
    printf("%d fixtures\n", g_nfx);

    /* Fixtures must be authored in non-decreasing timestamp order, or the
     * index refuses the feed and every sweep below would be a build error
     * rather than a comparison. */
    for (int x = 0; x < g_nfx; x++)
        for (int i = 1; i < g_fx[x].n; i++)
            if (g_fx[x].ev[i].timestamp_ns < g_fx[x].ev[i - 1].timestamp_ns)
                CHECK(0, "%s: fixture record %d goes backwards in time",
                      g_fx[x].name, i);

    if (!g_force_one_block) {
        literal_expectations();
        prewindow_equivalence();
    }
    sweep_cuts();
    if (!g_force_one_block) {
        comparator_probes();
        bypass_suite();
        measure_compaction();
    }
    non_vacuity();

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    printf("%s\n", tests_failed ? "FAILED" : "PASSED");
    return tests_failed ? 1 : 0;
}
