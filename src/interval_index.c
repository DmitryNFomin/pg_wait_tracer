/* interval_index.c — per-pid wait-interval index (paint-latency plan, Phase 4).
 *
 * Read interval_index.h first: the boundary rule, the window semantics and
 * the three silent-wrong modes are argued there, not here.
 *
 * What this file is NOT: it is not a second implementation of concurrency.
 * Bucketing, the peak tie-break, the sliding burst pass, the per-bucket
 * burst choice and every allocation refusal stay in
 * pgwt_compute_concurrency(), which this file CALLS. The only thing here is
 * selection: which intervals exist, which block stores each one, and which
 * of them a window selects. That is also the only thing the unit test's
 * differential can catch a mistake in — deliberately, because a
 * reimplementation would have to be differentialed against itself.
 */
#include "interval_index.h"
#include "idle_rule.h"
#include "test_alloc_fail.h"

#include <stdlib.h>
#include <string.h>

const char *pgwt_interval_index_refusal_str(enum pgwt_interval_index_refusal r)
{
    switch (r) {
    case PGWT_INTERVAL_INDEX_OK:                return "ok";
    case PGWT_INTERVAL_INDEX_REFUSE_NULL:       return "null-argument";
    case PGWT_INTERVAL_INDEX_REFUSE_VERSION:    return "version-mismatch";
    case PGWT_INTERVAL_INDEX_REFUSE_UNSEALED:   return "unsealed";
    case PGWT_INTERVAL_INDEX_REFUSE_BUILD_FAILED: return "build-failed";
    case PGWT_INTERVAL_INDEX_REFUSE_EMPTY:      return "empty-index";
    case PGWT_INTERVAL_INDEX_REFUSE_RANGE:      return "window-outside-coverage";
    case PGWT_INTERVAL_INDEX_REFUSE_BAD_WINDOW: return "bad-window";
    case PGWT_INTERVAL_INDEX_REFUSE_BAD_BUCKETS: return "bad-bucket-count";
    case PGWT_INTERVAL_INDEX_REFUSE_GAP:        return "block-gap-or-reorder";
    case PGWT_INTERVAL_INDEX_REFUSE_ALLOC:      return "allocation-failure";
    case PGWT_INTERVAL_INDEX_REFUSE_FILTERED:   return "filtered-request";
    case PGWT_INTERVAL_INDEX_REFUSE_NO_INTERVALS: return "no-intervals-selected";
    case PGWT_INTERVAL_INDEX_REFUSE_COMPUTE:    return "compute-failed";
    }
    return "unknown";
}

void pgwt_interval_index_init(struct pgwt_interval_index *idx)
{
    if (!idx)
        return;
    memset(idx, 0, sizeof(*idx));
    idx->version = PGWT_INTERVAL_INDEX_VERSION;
}

void pgwt_interval_index_free(struct pgwt_interval_index *idx)
{
    if (!idx)
        return;
    free(idx->rows);
    free(idx->chunks);
    memset(idx, 0, sizeof(*idx));
}

/* Latch a permanent build failure: a half-built index must never answer. */
static int fail_build(struct pgwt_interval_index *idx,
                      enum pgwt_interval_index_refusal why)
{
    idx->failed = 1;
    if (idx->build_refusal == PGWT_INTERVAL_INDEX_OK)
        idx->build_refusal = why;
    idx->sealed = 0;
    return -1;
}

int pgwt_interval_index_stores(const struct pgwt_filter *f,
                               const struct pgwt_trace_event *ev)
{
    if (!f || !ev)
        return 0;
    /* compute.c's concurrency_qualifies(), minus its two window
     * comparisons. The two helpers are the SHIPPING ones:
     * pgwt_filter_matches is the FID-4 chokepoint (it refuses markers and
     * refuses — never repairs — a record whose duration exceeds its own end
     * timestamp, which is what makes `timestamp_ns - duration_ns` below
     * safe), and pgwt_is_idle_event is the shared idle rule. */
    if (!pgwt_filter_matches(f, ev) || pgwt_is_idle_event(ev->old_event))
        return 0;
    if (ev->old_event == 0)
        return 0;       /* on-CPU: not a wait, never part of a burst */
    return 1;
}

static int rows_reserve(struct pgwt_interval_index *idx, int need)
{
    if (idx->n_rows + need <= idx->cap_rows)
        return 0;
    if (pgwt_test_alloc_fail("interval_index_rows"))
        return -1;
    int nc = idx->cap_rows ? idx->cap_rows * 2 : 256;
    while (nc < idx->n_rows + need)
        nc *= 2;
    struct pgwt_wait_interval *tmp = (struct pgwt_wait_interval *)
        realloc(idx->rows, (size_t)nc * sizeof(*tmp));
    if (!tmp)
        return -1;
    idx->rows = tmp;
    idx->cap_rows = nc;
    return 0;
}

static int chunks_reserve(struct pgwt_interval_index *idx)
{
    if (idx->n_chunks + 1 <= idx->cap_chunks)
        return 0;
    if (pgwt_test_alloc_fail("interval_index_chunks"))
        return -1;
    int nc = idx->cap_chunks ? idx->cap_chunks * 2 : 16;
    struct pgwt_interval_index_chunk *tmp = (struct pgwt_interval_index_chunk *)
        realloc(idx->chunks, (size_t)nc * sizeof(*tmp));
    if (!tmp)
        return -1;
    idx->chunks = tmp;
    idx->cap_chunks = nc;
    return 0;
}

int pgwt_interval_index_add_block(struct pgwt_interval_index *idx,
                                  int block_idx,
                                  uint64_t block_first_ns,
                                  uint64_t block_last_ns,
                                  const struct pgwt_trace_event *events,
                                  int count)
{
    if (!idx)
        return -1;
    if (idx->version != PGWT_INTERVAL_INDEX_VERSION)
        return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_VERSION);
    if (idx->failed)
        return -1;
    if (count < 0 || (count > 0 && !events))
        return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_NULL);
    /* The feed must be consecutive and non-decreasing. Both are refusals,
     * not repairs: the answer depends on record order (peak tie-break), and
     * a skipped block would make coverage a lie. */
    if (block_idx != idx->next_block_idx)
        return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_GAP);
    if (block_first_ns > block_last_ns)
        return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_GAP);
    if (idx->n_chunks > 0 &&
        block_first_ns < idx->chunks[idx->n_chunks - 1].block_last_ns)
        return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_GAP);

    if (chunks_reserve(idx) < 0)
        return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_ALLOC);

    struct pgwt_interval_index_chunk *ch = &idx->chunks[idx->n_chunks];
    memset(ch, 0, sizeof(*ch));
    ch->block_idx = block_idx;
    ch->first_row = idx->n_rows;
    ch->block_first_ns = block_first_ns;
    ch->block_last_ns = block_last_ns;

    /* The unfiltered filter: version 1 indexes the record set no filter
     * removes, and a filtered query refuses. */
    struct pgwt_filter unfiltered;
    memset(&unfiltered, 0, sizeof(unfiltered));

    uint64_t last_ts = idx->last_event_ns;
    int have_last = idx->have_last_event;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (ev->timestamp_ns < block_first_ns ||
            ev->timestamp_ns > block_last_ns)
            return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_GAP);
        if (have_last && ev->timestamp_ns < last_ts)
            return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_GAP);
        last_ts = ev->timestamp_ns;
        have_last = 1;
        idx->n_records_fed++;

        if (!pgwt_interval_index_stores(&unfiltered, ev)) {
            idx->n_records_skipped++;
            continue;
        }
        if (rows_reserve(idx, 1) < 0)
            return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_ALLOC);
        struct pgwt_wait_interval *iv = &idx->rows[idx->n_rows++];
        iv->end_ns = ev->timestamp_ns;
        /* THE TRUE START, UNCLIPPED. Not max(start, block_first_ns), not
         * max(start, cover_from_ns): see interval_index.h. Safe because
         * pgwt_filter_matches already refused duration_ns > timestamp_ns. */
        iv->start_ns = ev->timestamp_ns - ev->duration_ns;
        iv->pid = ev->pid;
        iv->event_id = ev->old_event;

        idx->n_intervals++;
        /* Straddles an INTERNAL block boundary: this is not the first block
         * and the interval began before that block opened. Deliberately
         * excludes the first block, where "starts before block_first_ns"
         * only means "starts before the indexed coverage"
         * (n_start_before_cover's job) — a one-block feed must drive this
         * counter to 0, or a suite that never splits a block could satisfy
         * it anyway. */
        if (idx->n_chunks > 0 && iv->start_ns < block_first_ns)
            idx->n_cross_block++;
    }

    ch->n_rows = idx->n_rows - ch->first_row;
    if (ch->n_rows > 0) {
        /* Rows are appended in fed order and the feed is non-decreasing in
         * timestamp_ns, so the first and last rows carry the extremes.
         * seal() re-verifies the invariant rather than trusting it. */
        ch->min_end_ns = idx->rows[ch->first_row].end_ns;
        ch->max_end_ns = idx->rows[ch->first_row + ch->n_rows - 1].end_ns;
    } else {
        ch->min_end_ns = UINT64_MAX;    /* overlaps no window */
        ch->max_end_ns = 0;
    }

    idx->last_event_ns = last_ts;
    idx->have_last_event = have_last;
    if (idx->n_chunks == 0)
        idx->cover_from_ns = block_first_ns;
    idx->cover_to_ns = block_last_ns;
    idx->n_chunks++;
    idx->next_block_idx = block_idx + 1;
    idx->sealed = 0;
    return 0;
}

int pgwt_interval_index_seal(struct pgwt_interval_index *idx)
{
    if (!idx)
        return -1;
    if (idx->version != PGWT_INTERVAL_INDEX_VERSION)
        return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_VERSION);
    if (idx->failed)
        return -1;
    if (idx->n_chunks == 0) {
        /* Sealing an empty index is allowed so the caller has one code
         * path; the query still REFUSES with REFUSE_EMPTY. "No index" must
         * never read as "no concurrency". */
        idx->sealed = 1;
        return 0;
    }

    /* The invariant the query's binary search and the peak tie-break both
     * rest on, verified rather than assumed. A violation is a REFUSAL: this
     * module never sorts the feed, because reordering would change
     * peak_event[] relative to the raw path. */
    for (int i = 1; i < idx->n_rows; i++)
        if (idx->rows[i].end_ns < idx->rows[i - 1].end_ns)
            return fail_build(idx, PGWT_INTERVAL_INDEX_REFUSE_GAP);

    idx->n_start_before_cover = 0;
    for (int i = 0; i < idx->n_rows; i++)
        if (idx->rows[i].start_ns < idx->cover_from_ns)
            idx->n_start_before_cover++;

    idx->sealed = 1;
    return 0;
}

static int is_unfiltered(const struct pgwt_filter *f)
{
    return f->class_name[0] == '\0' && f->event_id == 0 && f->pid == 0 &&
           f->query_id == 0;
}

/* First row of [lo, hi) whose end_ns >= from_ns. The chunk's rows are
 * non-decreasing in end_ns (verified at seal), so this is exact. */
static int lower_bound_end(const struct pgwt_wait_interval *rows,
                           int lo, int hi, uint64_t from_ns)
{
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (rows[mid].end_ns < from_ns)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* ONE walk, used by the counting pass and the filling pass, so the two can
 * never disagree about how many intervals there are (compute.c makes the
 * same argument about its own two passes). `fill` NULL = count only;
 * `stats` NULL = do not accumulate. Returns the number selected. */
static long select_walk(const struct pgwt_interval_index *idx,
                        uint64_t from_ns, uint64_t to_ns,
                        struct pgwt_trace_event *fill,
                        struct pgwt_interval_index_query_result *stats)
{
    long n = 0;
    for (int c = 0; c < idx->n_chunks; c++) {
        const struct pgwt_interval_index_chunk *ch = &idx->chunks[c];
        /* Prefilter on the SELECTION key (end_ns), inclusive at both ends —
         * the loader's own predicate. Narrowing either comparison to
         * half-open would skip a chunk holding a record the raw path keeps:
         * an event ending exactly at `to` lies wholly inside the window. */
        if (ch->n_rows == 0 || ch->max_end_ns < from_ns ||
            ch->min_end_ns > to_ns)
            continue;
        if (stats)
            stats->chunks_scanned++;

        int end = ch->first_row + ch->n_rows;
        int lb = lower_bound_end(idx->rows, ch->first_row, end, from_ns);
        if (stats)
            stats->excluded_end_before_from += lb - ch->first_row;

        for (int i = lb; i < end; i++) {
            const struct pgwt_wait_interval *iv = &idx->rows[i];
            if (iv->end_ns > to_ns) {
                /* Still waiting at `to`: its end record is outside the
                 * window, so the raw path does not select it either. Every
                 * remaining row of this chunk ends later still. */
                if (stats)
                    stats->excluded_open_past_to += end - i;
                break;
            }
            if (stats)
                stats->rows_examined++;
            if (fill) {
                struct pgwt_trace_event *e = &fill[n];
                memset(e, 0, sizeof(*e));
                e->timestamp_ns = iv->end_ns;
                e->duration_ns = iv->end_ns - iv->start_ns;
                e->pid = iv->pid;
                e->old_event = iv->event_id;
                e->new_event = 0;          /* the wait ended */
                e->cpu_ns = PGWT_CPU_NS_UNKNOWN;
            }
            n++;
        }
    }
    return n;
}

void pgwt_interval_index_query_free(
        struct pgwt_interval_index_query_result *out)
{
    if (!out)
        return;
    free(out->result.peak_sessions);
    free(out->result.peak_event);
    free(out->result.bursts);
    memset(&out->result, 0, sizeof(out->result));
    out->have_result = 0;
}

static int query_refuse(struct pgwt_interval_index_query_result *out,
                        enum pgwt_interval_index_refusal why)
{
    if (out) {
        memset(out, 0, sizeof(*out));
        out->refused = why;
    }
    return -1;
}

int pgwt_interval_index_query(const struct pgwt_interval_index *idx,
                              const struct pgwt_filter *f,
                              uint64_t from_ns, uint64_t to_ns,
                              int num_buckets,
                              uint64_t burst_window_ns, int burst_threshold,
                              struct pgwt_interval_index_query_result *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (!idx || !f)
        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_NULL);
    if (idx->version != PGWT_INTERVAL_INDEX_VERSION)
        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_VERSION);
    if (idx->failed)
        return query_refuse(out, idx->build_refusal != PGWT_INTERVAL_INDEX_OK
                                 ? idx->build_refusal
                                 : PGWT_INTERVAL_INDEX_REFUSE_BUILD_FAILED);
    if (!idx->sealed)
        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_UNSEALED);
    if (idx->n_chunks == 0)
        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_EMPTY);
    /* Version 1 indexes the UNFILTERED record set. A filter changes which
     * records qualify, so a filtered request falls back to raw instead of
     * being answered from the wrong set. */
    if (!is_unfiltered(f))
        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_FILTERED);
    /* pgwt_compute_concurrency returns a ZEROED result for these, which
     * would serialise as "no concurrency". Refuse instead. */
    if (from_ns >= to_ns)
        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_BAD_WINDOW);
    if (num_buckets <= 0)
        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_BAD_BUCKETS);
    /* CONTAINMENT, not intersection: the uncovered part of the window has no
     * index, and "no index" must never answer "no waits". The caller clamps
     * to cover_to_ns and decodes the remainder (plan rule 4). */
    if (from_ns < idx->cover_from_ns || to_ns > idx->cover_to_ns)
        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_RANGE);

    out->chunks_total = idx->n_chunks;

    long nsel = select_walk(idx, from_ns, to_ns, NULL, out);
    /* See "FALL BACK, NEVER GUESS" in interval_index.h: with zero intervals
     * the index cannot tell an empty raw array (count == 0 -> no buckets at
     * all) from a non-empty one in which nothing qualified (count > 0 -> N
     * zero buckets), and the two serialise differently. */
    if (nsel == 0)
        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_NO_INTERVALS);

    struct pgwt_trace_event *arr = NULL;
    if (!pgwt_test_alloc_fail("interval_index_query_events"))
        arr = (struct pgwt_trace_event *)malloc((size_t)nsel * sizeof(*arr));
    if (!arr)
        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_ALLOC);

    long nfill = select_walk(idx, from_ns, to_ns, arr, NULL);
    if (nfill != nsel) {
        /* Unreachable: one walk, one predicate. Refuse rather than compute
         * over a partially filled array. */
        free(arr);
        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_ALLOC);
    }
    out->intervals_used = nsel;
    out->bytes_materialised = (long)((size_t)nsel * sizeof(*arr));

    /* THE SHIPPING COMPUTATION. Not a copy of it. */
    pgwt_compute_concurrency(arr, (int)nsel, f, from_ns, to_ns, num_buckets,
                             burst_window_ns, burst_threshold, &out->result);
    free(arr);

    if (out->result.failed) {
        pgwt_interval_index_query_free(out);
        return query_refuse(out, PGWT_INTERVAL_INDEX_REFUSE_COMPUTE);
    }
    out->have_result = 1;
    out->refused = PGWT_INTERVAL_INDEX_OK;
    return 0;
}
