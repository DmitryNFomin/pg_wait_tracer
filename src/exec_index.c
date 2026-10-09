/* exec_index.c — append-only executions index (paint-latency plan, Phase 3).
 *
 * The state machine below is an INDEPENDENT reimplementation of the marker
 * handling in pgwt_compute_executions() (src/compute.c), not a refactor of
 * it and not a copy of it. That is deliberate: the unit test's oracle is
 * the real pgwt_compute_executions, so a differential between the two
 * catches a mistake in either. A shared helper would make the differential
 * vacuous.
 *
 * Marker semantics replicated here, each one matched against the reference
 * line by line (src/compute.c, pgwt_compute_executions):
 *
 *   PLAN_START  record plan_start_ns/plan_query_id, plan_open = 1.
 *   PLAN_END    only when plan_open AND ts >= plan_start_ns does a plan
 *               become "ready"; ready_plan_query_id falls back to the
 *               PLAN_END's own query_id when PLAN_START carried none.
 *               plan_open = 0 either way.
 *   EXEC_START  PUSH a new row (never overwrite — #222: a single
 *               active_row loses the outer row on the second start).
 *               Attach a ready plan when its query id is compatible.
 *               Consume plan_ready unconditionally.
 *   EXEC_END    POP the innermost row; close it only when ts >= its
 *               start_ns. A pop whose guard fails leaves that row open
 *               FOREVER (it is off the stack) — replicated, not fixed.
 *               Fills a zero query_id from the marker.
 *   CMD_END     POP and close EVERY still-open row for the pid, marking
 *               close_inferred. Clears plan_open and plan_ready.
 *
 * `top_attributable` (the reference's per-pid "is the top-of-stack row the
 * one that just started" bit) is deliberately ABSENT here. Its only reads
 * in the reference are exec_pid_attributable_row() calls in the non-marker
 * branch, and its only effects are n_events, n_workers and
 * matches_event_filter — the three fields this index does not carry (see
 * NOT INDEXED in exec_index.h). Nothing in a boundary field depends on it.
 */
#include "exec_index.h"
#include "pid_index.h"
#include "test_alloc_fail.h"

#include <stdlib.h>
#include <string.h>

/* Per-pid builder state. THIS is the open-execution carry-over: it lives in
 * the index, not in a per-block local, so an execution opened in block 3 and
 * closed in block 9 is one row with both boundaries, and one still open at
 * capture end stays open rather than being invented a boundary. */
struct exec_pid_carry {
    uint32_t pid;
    int     *open_rows;             /* LIFO of row indices, innermost last */
    int      n_open, cap_open;
    uint64_t plan_start_ns, plan_query_id;
    int      plan_open;
    uint64_t ready_plan_start_ns, ready_plan_end_ns, ready_plan_query_id;
    int      plan_ready;
};

const char *pgwt_exec_index_refusal_str(enum pgwt_exec_index_refusal r)
{
    switch (r) {
    case PGWT_EXEC_INDEX_OK:             return "ok";
    case PGWT_EXEC_INDEX_REFUSE_NULL:    return "null-argument";
    case PGWT_EXEC_INDEX_REFUSE_VERSION: return "version-mismatch";
    case PGWT_EXEC_INDEX_REFUSE_UNSEALED: return "unsealed";
    case PGWT_EXEC_INDEX_REFUSE_BUILD_FAILED: return "build-failed";
    case PGWT_EXEC_INDEX_REFUSE_EMPTY:   return "empty-index";
    case PGWT_EXEC_INDEX_REFUSE_RANGE:   return "window-outside-coverage";
    case PGWT_EXEC_INDEX_REFUSE_BAD_WINDOW: return "bad-window";
    case PGWT_EXEC_INDEX_REFUSE_GAP:     return "block-gap-or-reorder";
    case PGWT_EXEC_INDEX_REFUSE_ALLOC:   return "allocation-failure";
    }
    return "unknown";
}

void pgwt_exec_index_init(struct pgwt_exec_index *idx)
{
    if (!idx)
        return;
    memset(idx, 0, sizeof(*idx));
    idx->version = PGWT_EXEC_INDEX_VERSION;
    idx->cover_from_ns = 0;
    idx->cover_to_ns = 0;
    idx->next_block_idx = 0;
}

void pgwt_exec_index_free(struct pgwt_exec_index *idx)
{
    if (!idx)
        return;
    struct exec_pid_carry *pids = (struct exec_pid_carry *)idx->pids;
    for (int i = 0; i < idx->n_pids; i++)
        free(pids[i].open_rows);
    free(pids);
    if (idx->pid_ix) {
        pgwt_pid_index_free((struct pgwt_pid_index *)idx->pid_ix);
        free(idx->pid_ix);
    }
    free(idx->rows);
    free(idx->chunks);
    memset(idx, 0, sizeof(*idx));
}

/* Latch a permanent build failure. Everything downstream refuses with this
 * reason: a half-built index must never answer a query. */
static int fail_build(struct pgwt_exec_index *idx,
                      enum pgwt_exec_index_refusal why)
{
    idx->failed = 1;
    if (idx->build_refusal == PGWT_EXEC_INDEX_OK)
        idx->build_refusal = why;
    idx->sealed = 0;
    return -1;
}

static int carry_get(struct pgwt_exec_index *idx, uint32_t pid)
{
    if (!idx->pid_ix) {
        struct pgwt_pid_index *ix =
            (struct pgwt_pid_index *)malloc(sizeof(*ix));
        if (!ix)
            return -1;
        pgwt_pid_index_init(ix);
        idx->pid_ix = ix;
    }
    struct pgwt_pid_index *ix = (struct pgwt_pid_index *)idx->pid_ix;
    int slot = pgwt_pid_index_find(ix, pid);
    if (slot >= 0)
        return slot;

    if (idx->n_pids >= idx->cap_pids) {
        if (pgwt_test_alloc_fail("exec_index_pids"))
            return -1;
        int nc = idx->cap_pids ? idx->cap_pids * 2 : 64;
        struct exec_pid_carry *tmp = (struct exec_pid_carry *)
            realloc(idx->pids, (size_t)nc * sizeof(*tmp));
        if (!tmp)
            return -1;
        idx->pids = tmp;
        idx->cap_pids = nc;
    }
    int at = idx->n_pids;
    struct exec_pid_carry *pids = (struct exec_pid_carry *)idx->pids;
    memset(&pids[at], 0, sizeof(pids[at]));
    pids[at].pid = pid;
    /* The map is unbounded (#275): it grows or it FAILS, never drops. */
    if (pgwt_pid_index_put(ix, pid, at) < 0)
        return -1;
    idx->n_pids++;
    return at;
}

static int carry_push(struct exec_pid_carry *st, int row_idx)
{
    if (st->n_open >= st->cap_open) {
        if (pgwt_test_alloc_fail("exec_index_open_rows"))
            return -1;
        int nc = st->cap_open ? st->cap_open * 2 : 4;
        int *tmp = (int *)realloc(st->open_rows, (size_t)nc * sizeof(*tmp));
        if (!tmp)
            return -1;
        st->open_rows = tmp;
        st->cap_open = nc;
    }
    st->open_rows[st->n_open++] = row_idx;
    return 0;
}

static int carry_pop(struct exec_pid_carry *st)
{
    return st->n_open > 0 ? st->open_rows[--st->n_open] : -1;
}

static int rows_append(struct pgwt_exec_index *idx,
                       const struct pgwt_exec_index_entry *e)
{
    if (idx->n_rows >= idx->cap_rows) {
        if (pgwt_test_alloc_fail("exec_index_rows"))
            return -1;
        int nc = idx->cap_rows ? idx->cap_rows * 2 : 128;
        struct pgwt_exec_index_entry *tmp = (struct pgwt_exec_index_entry *)
            realloc(idx->rows, (size_t)nc * sizeof(*tmp));
        if (!tmp)
            return -1;
        idx->rows = tmp;
        idx->cap_rows = nc;
    }
    idx->rows[idx->n_rows] = *e;
    return idx->n_rows++;
}

int pgwt_exec_index_add_block(struct pgwt_exec_index *idx, int block_idx,
                              uint64_t block_first_ns, uint64_t block_last_ns,
                              const struct pgwt_trace_event *events, int count)
{
    if (!idx)
        return -1;
    if (idx->version != PGWT_EXEC_INDEX_VERSION)
        return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_VERSION);
    if (idx->failed)
        return -1;
    if (count < 0 || (count > 0 && !events))
        return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_NULL);

    /* A partial or reordered block range must REFUSE, not answer. The
     * caller's own running counter is the only thing that can tell us a
     * block was skipped: the events of a skipped block are simply not here,
     * and a missing EXEC_END is indistinguishable from a still-open
     * execution. #317's lesson in a different costume. */
    if (block_idx != idx->next_block_idx)
        return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_GAP);
    if (block_last_ns < block_first_ns)
        return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_GAP);
    if (idx->n_chunks > 0) {
        const struct pgwt_exec_index_chunk *prev =
            &idx->chunks[idx->n_chunks - 1];
        if (block_first_ns < prev->block_first_ns ||
            block_last_ns < prev->block_last_ns)
            return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_GAP);
    }

    /* Grow the chunk array BEFORE touching per-pid state, so a failure here
     * cannot leave the carry-over half-advanced. */
    if (idx->n_chunks >= idx->cap_chunks) {
        if (pgwt_test_alloc_fail("exec_index_chunks"))
            return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_ALLOC);
        int nc = idx->cap_chunks ? idx->cap_chunks * 2 : 32;
        struct pgwt_exec_index_chunk *tmp = (struct pgwt_exec_index_chunk *)
            realloc(idx->chunks, (size_t)nc * sizeof(*tmp));
        if (!tmp)
            return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_ALLOC);
        idx->chunks = tmp;
        idx->cap_chunks = nc;
    }
    struct pgwt_exec_index_chunk *ch = &idx->chunks[idx->n_chunks];
    memset(ch, 0, sizeof(*ch));
    ch->block_idx = block_idx;
    ch->first_row = idx->n_rows;
    ch->block_first_ns = block_first_ns;
    ch->block_last_ns = block_last_ns;

    /* Global stream order across the whole feed (not just within a block):
     * the oracle sees one ordered array, so anything else is a different
     * input. Seeded from the previous block's last record. */
    uint64_t last_ts = idx->last_event_ns;
    int have_last = idx->have_last_event;

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (ev->timestamp_ns < block_first_ns ||
            ev->timestamp_ns > block_last_ns)
            return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_GAP);
        if (have_last && ev->timestamp_ns < last_ts)
            return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_GAP);
        last_ts = ev->timestamp_ns;
        have_last = 1;

        uint32_t marker = ev->old_event;
        if (marker != PGWT_MARKER_PLAN_START &&
            marker != PGWT_MARKER_PLAN_END &&
            marker != PGWT_MARKER_EXEC_START &&
            marker != PGWT_MARKER_EXEC_END &&
            marker != PGWT_MARKER_CMD_END)
            continue;   /* every other record, marker or not, is inert here */

        int ci = carry_get(idx, ev->pid);
        if (ci < 0)
            return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_ALLOC);
        struct exec_pid_carry *st = &((struct exec_pid_carry *)idx->pids)[ci];

        if (marker == PGWT_MARKER_PLAN_START) {
            st->plan_start_ns = ev->timestamp_ns;
            st->plan_query_id = ev->query_id;
            st->plan_open = 1;
            continue;
        }
        if (marker == PGWT_MARKER_PLAN_END) {
            if (st->plan_open && ev->timestamp_ns >= st->plan_start_ns) {
                st->ready_plan_start_ns = st->plan_start_ns;
                st->ready_plan_end_ns = ev->timestamp_ns;
                st->ready_plan_query_id = st->plan_query_id
                                        ? st->plan_query_id : ev->query_id;
                st->plan_ready = 1;
            }
            st->plan_open = 0;
            continue;
        }
        if (marker == PGWT_MARKER_EXEC_START) {
            struct pgwt_exec_index_entry e;
            memset(&e, 0, sizeof(e));
            e.pid = ev->pid;
            e.query_id = ev->query_id;
            e.start_ns = ev->timestamp_ns;
            if (st->plan_ready &&
                (st->ready_plan_query_id == 0 || e.query_id == 0 ||
                 st->ready_plan_query_id == e.query_id)) {
                e.plan_start_ns = st->ready_plan_start_ns;
                e.plan_end_ns = st->ready_plan_end_ns;
                e.has_plan = 1;
            }
            int row_idx = rows_append(idx, &e);
            if (row_idx < 0)
                return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_ALLOC);
            /* carry_push may realloc nothing that invalidates `st` (st
             * points into idx->pids, which carry_push never touches), but
             * re-fetch anyway so this stays true if either grows later. */
            st = &((struct exec_pid_carry *)idx->pids)[ci];
            if (carry_push(st, row_idx) < 0)
                return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_ALLOC);
            st->plan_ready = 0;
            continue;
        }
        if (marker == PGWT_MARKER_EXEC_END) {
            int row_idx = carry_pop(st);
            if (row_idx >= 0 && row_idx < idx->n_rows &&
                ev->timestamp_ns >= idx->rows[row_idx].start_ns) {
                struct pgwt_exec_index_entry *e = &idx->rows[row_idx];
                e->closed = 1;
                e->close_ns = ev->timestamp_ns;
                e->close_inferred = 0;
                e->close_query_id = ev->query_id;
            }
            continue;
        }
        /* CMD_END */
        {
            int row_idx;
            while ((row_idx = carry_pop(st)) >= 0) {
                if (row_idx < idx->n_rows && !idx->rows[row_idx].closed &&
                    ev->timestamp_ns >= idx->rows[row_idx].start_ns) {
                    struct pgwt_exec_index_entry *e = &idx->rows[row_idx];
                    e->closed = 1;
                    e->close_ns = ev->timestamp_ns;
                    e->close_inferred = 1;
                    e->close_query_id = 0;   /* reference never fills it here */
                }
            }
            st->plan_open = 0;
            st->plan_ready = 0;
        }
    }

    ch->n_rows = idx->n_rows - ch->first_row;
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

int pgwt_exec_index_seal(struct pgwt_exec_index *idx)
{
    if (!idx)
        return -1;
    if (idx->version != PGWT_EXEC_INDEX_VERSION)
        return fail_build(idx, PGWT_EXEC_INDEX_REFUSE_VERSION);
    if (idx->failed)
        return -1;
    if (idx->n_chunks == 0) {
        /* Nothing was indexed. Sealing is allowed so the caller has one
         * code path, but the query still REFUSES with REFUSE_EMPTY: an
         * empty index must never read as "this window has no executions". */
        idx->sealed = 1;
        return 0;
    }

    /* Finalise the prefilter bounds. This cannot be done incrementally in
     * add_block(): a row opened in chunk k may be closed by a marker in
     * chunk k+n, so its end is only known once the feed is complete. */
    for (int c = 0; c < idx->n_chunks; c++) {
        struct pgwt_exec_index_chunk *ch = &idx->chunks[c];
        if (ch->n_rows == 0) {
            /* An empty chunk can never contribute a row, so give it bounds
             * that overlap nothing. */
            ch->min_start_ns = UINT64_MAX;
            ch->max_end_ns = 0;
            continue;
        }
        uint64_t mn = UINT64_MAX, mx = 0;
        for (int i = ch->first_row; i < ch->first_row + ch->n_rows; i++) {
            const struct pgwt_exec_index_entry *e = &idx->rows[i];
            if (e->start_ns < mn)
                mn = e->start_ns;
            /* Still open at capture end: it can overlap ANY later window,
             * so it must never be prefiltered away. */
            uint64_t end = e->closed ? e->close_ns : UINT64_MAX;
            if (end > mx)
                mx = end;
        }
        ch->min_start_ns = mn;
        ch->max_end_ns = mx;
    }
    idx->sealed = 1;
    return 0;
}

int pgwt_exec_index_open_at_end(const struct pgwt_exec_index *idx)
{
    if (!idx || idx->failed || !idx->sealed ||
        idx->version != PGWT_EXEC_INDEX_VERSION)
        return -1;
    int n = 0;
    for (int i = 0; i < idx->n_rows; i++)
        if (!idx->rows[i].closed)
            n++;
    return n;
}

int pgwt_exec_index_retains(uint64_t start_ns, uint64_t end_ns, int in_progress,
                            uint64_t from_ns, uint64_t to_ns)
{
    /* Verbatim from pgwt_compute_executions' compaction loop:
     *     if (rows[i].start_ns > to_ns) continue;
     *     if (!rows[i].in_progress && rows[i].end_ns < from_ns) continue;
     * Inclusive at both ends. See HALF-OPEN in exec_index.h. */
    if (start_ns > to_ns)
        return 0;
    if (!in_progress && end_ns < from_ns)
        return 0;
    return 1;
}

void pgwt_exec_index_query_free(struct pgwt_exec_index_query_result *out)
{
    if (!out)
        return;
    free(out->rows);
    out->rows = NULL;
    out->num_rows = 0;
}

static int query_refuse(struct pgwt_exec_index_query_result *out,
                        enum pgwt_exec_index_refusal why)
{
    if (out) {
        memset(out, 0, sizeof(*out));
        out->refused = why;
    }
    return -1;
}

int pgwt_exec_index_query(const struct pgwt_exec_index *idx,
                          uint64_t from_ns, uint64_t to_ns,
                          struct pgwt_exec_index_query_result *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (!idx)
        return query_refuse(out, PGWT_EXEC_INDEX_REFUSE_NULL);
    if (idx->version != PGWT_EXEC_INDEX_VERSION)
        return query_refuse(out, PGWT_EXEC_INDEX_REFUSE_VERSION);
    if (idx->failed)
        return query_refuse(out, idx->build_refusal != PGWT_EXEC_INDEX_OK
                                 ? idx->build_refusal
                                 : PGWT_EXEC_INDEX_REFUSE_BUILD_FAILED);
    if (!idx->sealed)
        return query_refuse(out, PGWT_EXEC_INDEX_REFUSE_UNSEALED);
    if (idx->n_chunks == 0)
        return query_refuse(out, PGWT_EXEC_INDEX_REFUSE_EMPTY);
    if (from_ns > to_ns)
        return query_refuse(out, PGWT_EXEC_INDEX_REFUSE_BAD_WINDOW);
    /* Containment, not intersection: the uncovered part of the window has
     * no index, and "no index" must never answer "no executions". The
     * caller clamps and decodes the remainder (plan rule 4). */
    if (from_ns < idx->cover_from_ns || to_ns > idx->cover_to_ns)
        return query_refuse(out, PGWT_EXEC_INDEX_REFUSE_RANGE);

    out->chunks_total = idx->n_chunks;
    out->counts_indexed = 0;

    struct pgwt_execution *rows = NULL;
    int n = 0, cap = 0;

    for (int c = 0; c < idx->n_chunks; c++) {
        const struct pgwt_exec_index_chunk *ch = &idx->chunks[c];
        /* Prefilter. Provably a SUPERSET of the retention predicate:
         *  - min_start_ns > to_ns  => every row in the chunk has
         *    start_ns > to_ns, so retains() drops all of them;
         *  - max_end_ns < from_ns  => every row is closed (an open row
         *    would have made max_end_ns UINT64_MAX) with
         *    close_ns <= max_end_ns < from_ns <= to_ns, so each is
         *    in_progress == 0 with end_ns < from_ns, and retains() drops
         *    all of them.
         * Half-open [from, to) is not usable here precisely because
         * retains() is inclusive; using it would skip a chunk holding a row
         * the reference keeps. */
        if (ch->n_rows == 0 || ch->min_start_ns > to_ns ||
            ch->max_end_ns < from_ns)
            continue;
        out->chunks_scanned++;

        for (int i = ch->first_row; i < ch->first_row + ch->n_rows; i++) {
            const struct pgwt_exec_index_entry *e = &idx->rows[i];
            out->rows_examined++;

            /* Window-relative close: a close marker past to_ns is not in
             * the reference's event array at all, so the row is reported
             * exactly as the reference reports an unclosed one. */
            int in_progress = (!e->closed || e->close_ns > to_ns);
            uint64_t end_ns = in_progress ? 0 : e->close_ns;
            if (!pgwt_exec_index_retains(e->start_ns, end_ns, in_progress,
                                         from_ns, to_ns))
                continue;

            if (n >= cap) {
                if (pgwt_test_alloc_fail("exec_index_query_rows")) {
                    free(rows);
                    return query_refuse(out, PGWT_EXEC_INDEX_REFUSE_ALLOC);
                }
                int nc = cap ? cap * 2 : 128;
                struct pgwt_execution *tmp = (struct pgwt_execution *)
                    realloc(rows, (size_t)nc * sizeof(*tmp));
                if (!tmp) {
                    free(rows);
                    return query_refuse(out, PGWT_EXEC_INDEX_REFUSE_ALLOC);
                }
                rows = tmp;
                cap = nc;
            }
            struct pgwt_execution *r = &rows[n++];
            memset(r, 0, sizeof(*r));
            r->pid = e->pid;
            r->query_id = e->query_id;
            if (r->query_id == 0 && !in_progress && !e->close_inferred)
                r->query_id = e->close_query_id;
            r->start_ns = e->start_ns;
            r->end_ns = end_ns;
            r->plan_start_ns = e->plan_start_ns;
            r->plan_end_ns = e->plan_end_ns;
            r->has_plan = e->has_plan;
            r->in_progress = in_progress;
            r->end_inferred = in_progress ? 0 : (int)e->close_inferred;
            r->started_before_window = e->start_ns < from_ns;
            /* NOT INDEXED — poisoned, never 0. See exec_index.h. */
            r->n_events = PGWT_EXEC_INDEX_NOT_INDEXED;
            r->n_workers = PGWT_EXEC_INDEX_NOT_INDEXED;
            r->matches_event_filter = PGWT_EXEC_INDEX_NOT_INDEXED;
        }
    }

    out->rows = rows;
    out->num_rows = n;
    out->refused = PGWT_EXEC_INDEX_OK;
    return 0;
}
