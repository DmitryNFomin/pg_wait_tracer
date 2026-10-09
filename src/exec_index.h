/* exec_index.h — append-only executions index (paint-latency plan, Phase 3).
 *
 * WHY THIS EXISTS
 * ---------------
 * Six detail commands load the full raw event window on every request, at
 * ~0.68 s per million records. Four of them — `executions`, `exec_scatter`,
 * `variants`, `waterfall` — all want the same thing: the BOUNDARIES of
 * individual query executions, delimited by the PLAN_START, PLAN_END,
 * EXEC_START, EXEC_END and CMD_END markers (writing those with a glob would
 * close this comment, which is why they are spelled out).
 * Today each re-walks every event in the window to rediscover
 * them. This index is built once per committed block, carried across block
 * boundaries, and answered per request in O(chunks + executions overlapping
 * the window) instead of O(events in the window).
 *
 * THE CONTRACT (version 1) — read this before using a single field
 * ---------------------------------------------------------------
 * `pgwt_exec_index_query(idx, from_ns, to_ns, out)` returns rows whose
 * BOUNDARY fields
 *
 *     pid, query_id, start_ns, end_ns, plan_start_ns, plan_end_ns,
 *     has_plan, in_progress, end_inferred, started_before_window
 *
 * are BIT-EXACTLY equal, row for row and in the same order, to those of
 *
 *     pgwt_compute_executions(E, n, from_ns, to_ns, f, links, n_links, &r)
 *
 * where E is any event array satisfying:
 *   (a) it contains every PGWT_IS_MARKER record of the indexed stream with
 *       timestamp_ns <= to_ns, in stream order, and
 *   (b) it contains no record with timestamp_ns > to_ns.
 * Non-marker records may be present or absent in any quantity: the boundary
 * fields above are a pure function of the marker subsequence (see MARKERS
 * ONLY below). That is exactly the shape src/server.c's
 * load_execution_rows() builds — a markers-only prefix below `from`,
 * concatenated with every event in [from, to] — so the contract covers the
 * real caller, and `f`/`links` do not appear in it because no boundary
 * field depends on them.
 *
 * MARKERS ONLY. In pgwt_compute_executions the non-marker branch reads the
 * per-pid state (exec_pid_attributable_row) and writes only n_events,
 * n_workers, matches_event_filter and its own worker_last_row scratch; it
 * mutates no open-row stack and no plan state. Therefore every field in the
 * list above is determined by the marker records alone. The unit test pins
 * this by building each fixture twice — full events, and markers only — and
 * requiring an identical index.
 *
 * NOT INDEXED, AND LOUDLY SO. n_events, n_workers and matches_event_filter
 * are per-event, per-filter and window-CLIPPED quantities
 * (interval_overlaps() in compute.c), so they cannot come out of a
 * boundary index. "Absence is never an answer and must never read as zero"
 * (plan rule 3), so the query does not leave them at 0: it writes the
 * poison value PGWT_EXEC_INDEX_NOT_INDEXED (-1) into all three and sets
 * out->counts_indexed = 0. A caller that needs any of them must recompute
 * that part from raw; a caller that reads the field anyway sees -1, which
 * is not a plausible count.
 *
 * HALF-OPEN vs THE REFERENCE'S RETENTION BOUNDS. The plan's rule 4
 * (half-open [from, to) at every seam, after #316) is applied to the
 * BLOCK-ASSIGNMENT and chunk-prefilter seams below, where a seam exists.
 * It is deliberately NOT applied to the per-row retention predicate:
 * pgwt_compute_executions keeps a row when `!(start_ns > to_ns)` and
 * `!(!in_progress && end_ns < from_ns)` — i.e. inclusive at BOTH ends —
 * and rule 1 (bit-exact, no tolerance) outranks tidiness. Narrowing it
 * here would make the index disagree with raw at the seam, which is the
 * failure this phase exists to avoid. pgwt_exec_index_retains() is that
 * predicate, written once, used by the query and available to tests.
 *
 * VERSION IS THE CONTRACT (#315). PGWT_EXEC_INDEX_VERSION is stamped into
 * every index at init and checked on every query. Everything the paragraphs
 * above claim is true of version 1 as shipped, and is pinned by
 * tests/test_exec_index.c. Bump the version in the same commit that changes
 * any of it.
 *
 * FALL BACK, NEVER GUESS. Every way the query can fail to establish the
 * answer returns a distinct enum pgwt_exec_index_refusal and produces NO
 * rows. `out->num_rows == 0 && out->refused == PGWT_EXEC_INDEX_OK` is the
 * only encoding of "this window genuinely contains no executions". In
 * particular an empty index, an unsealed index, a build that hit an
 * allocation failure, a window not fully covered by the indexed blocks, and
 * a non-consecutive or out-of-order block feed all REFUSE, so the caller
 * recomputes from raw instead of painting an empty tab.
 */
#ifndef PGWT_EXEC_INDEX_H
#define PGWT_EXEC_INDEX_H

#include "compute.h"
#include "pg_wait_tracer.h"

#include <stdint.h>

#define PGWT_EXEC_INDEX_VERSION 1

/* Poison written into every field the index does not carry. -1 is not a
 * plausible event or worker count, so a caller that forgets
 * out->counts_indexed cannot read absence as zero. */
#define PGWT_EXEC_INDEX_NOT_INDEXED (-1)

enum pgwt_exec_index_refusal {
    PGWT_EXEC_INDEX_OK = 0,
    PGWT_EXEC_INDEX_REFUSE_NULL,          /* NULL index or NULL out */
    PGWT_EXEC_INDEX_REFUSE_VERSION,       /* version != PGWT_EXEC_INDEX_VERSION */
    PGWT_EXEC_INDEX_REFUSE_UNSEALED,      /* seal() not called (or re-opened) */
    PGWT_EXEC_INDEX_REFUSE_BUILD_FAILED,  /* an allocation failed while building */
    PGWT_EXEC_INDEX_REFUSE_EMPTY,         /* no block was ever added */
    PGWT_EXEC_INDEX_REFUSE_RANGE,         /* window not contained in coverage */
    PGWT_EXEC_INDEX_REFUSE_BAD_WINDOW,    /* from_ns > to_ns */
    PGWT_EXEC_INDEX_REFUSE_GAP,           /* block feed skipped/reordered a block */
    PGWT_EXEC_INDEX_REFUSE_ALLOC,         /* query-side allocation failure */
};

const char *pgwt_exec_index_refusal_str(enum pgwt_exec_index_refusal r);

/* One indexed execution. Window-INDEPENDENT by construction: nothing here
 * is derived from a request's [from, to), which is what lets one index
 * answer every window.
 *
 * `closed`/`close_ns`/`close_inferred` record what the CAPTURE saw, not what
 * a window sees. The query derives the window-relative in_progress/end_ns
 * from them: a row whose close marker lies past `to_ns` is reported
 * in_progress with end_ns = 0, because that is precisely what
 * pgwt_compute_executions reports when its caller's event array stops at
 * to_ns and the closing marker is therefore absent. */
struct pgwt_exec_index_entry {
    uint32_t pid;
    uint64_t query_id;
    uint64_t start_ns;          /* the EXEC_START timestamp */
    uint64_t close_ns;          /* closing marker timestamp; 0 iff !closed */
    /* The closing EXEC_END's own query_id. pgwt_compute_executions fills a
     * row's zero query_id from it ("if (row->query_id == 0) row->query_id =
     * ev->query_id") — in the EXEC_END branch ONLY, never in the CMD_END
     * one. So this is applied at query time, and only when the close is
     * inside the window and was a real EXEC_END; a row whose close lies
     * past to_ns keeps query_id 0, exactly as the reference does when that
     * marker is not in its array. */
    uint64_t close_query_id;
    uint64_t plan_start_ns;
    uint64_t plan_end_ns;
    uint8_t  has_plan;
    uint8_t  closed;            /* a real closing marker was seen in capture */
    uint8_t  close_inferred;    /* closed by CMD_END, not by EXEC_END (#222) */
};

/* One add_block() call. The prefilter unit: a chunk whose [min_start_ns,
 * max_end_ns] cannot overlap the request window is skipped without touching
 * its rows, which is where O(events) becomes O(executions in window).
 *
 * max_end_ns is UINT64_MAX when any row in the chunk is still open at the
 * end of the capture — an open execution must never be prefiltered away.
 * Both bounds are finalised in seal(), because a row opened in this chunk
 * may be closed by a marker that arrives several chunks later. */
struct pgwt_exec_index_chunk {
    int      block_idx;
    int      first_row;
    int      n_rows;
    uint64_t block_first_ns, block_last_ns;
    uint64_t min_start_ns;
    uint64_t max_end_ns;
};

struct pgwt_exec_index {
    uint32_t version;
    int      sealed;
    int      failed;            /* an allocation failed while building */
    enum pgwt_exec_index_refusal build_refusal;

    struct pgwt_exec_index_entry *rows;
    int      n_rows, cap_rows;

    struct pgwt_exec_index_chunk *chunks;
    int      n_chunks, cap_chunks;

    /* Coverage: the union of the block time bounds actually fed in. A query
     * window that is not CONTAINED in it REFUSES (REFUSE_RANGE) — otherwise
     * a window reaching past the indexed range would answer "no executions"
     * for the part nobody indexed, which is the exact false negative plan
     * rule 3 forbids. These two fields are public so the caller can do what
     * plan rule 4 requires instead: clamp the index query to the covered
     * span and DECODE the boundary remainder from raw. */
    uint64_t cover_from_ns, cover_to_ns;
    int      next_block_idx;    /* the only block_idx add_block will accept */

    /* Global stream order across the whole feed. The oracle sees ONE
     * timestamp-ordered array, so an out-of-order record is a different
     * input, not a different index — it latches REFUSE_GAP. */
    uint64_t last_event_ns;
    int      have_last_event;

    /* Per-pid builder state, carried across add_block() calls. This IS the
     * open-execution carry-over. */
    void    *pids;              /* struct exec_pid_carry *, opaque */
    int      n_pids, cap_pids;
    void    *pid_ix;            /* struct pgwt_pid_index *, opaque */
};

/* Reset to an empty, unsealed, version-stamped index. */
void pgwt_exec_index_init(struct pgwt_exec_index *idx);
void pgwt_exec_index_free(struct pgwt_exec_index *idx);

/* Feed one committed block's decoded events, in stream order.
 *
 * `block_idx` must be the caller's running 0-based counter over the blocks
 * it is indexing; a value other than the expected next one latches
 * REFUSE_GAP, so a partial or reordered block range can never be queried as
 * if it were complete. `block_first_ns`/`block_last_ns` are the block's own
 * header bounds (NOT derived from `events`, so a block whose events were
 * all filtered out still extends coverage honestly); they must be
 * non-decreasing across calls.
 *
 * Events with timestamp_ns outside [block_first_ns, block_last_ns] are a
 * caller/file inconsistency and latch REFUSE_GAP rather than being clamped.
 *
 * Returns 0 on success, -1 on any failure (the index is then permanently
 * un-queryable and the query reports why). */
int pgwt_exec_index_add_block(struct pgwt_exec_index *idx, int block_idx,
                              uint64_t block_first_ns, uint64_t block_last_ns,
                              const struct pgwt_trace_event *events, int count);

/* Finalise chunk bounds. Must be called once, after the last add_block().
 * Returns 0 on success, -1 on failure. */
int pgwt_exec_index_seal(struct pgwt_exec_index *idx);

/* Executions still open at the end of the fed stream — the carry-over that
 * survived to capture end. -1 if the index is unusable. Exposed so a caller
 * (and the test) can see the count rather than infer it. */
int pgwt_exec_index_open_at_end(const struct pgwt_exec_index *idx);

struct pgwt_exec_index_query_result {
    /* malloc'd, free with pgwt_exec_index_query_free(). Boundary fields are
     * exact per the contract above; n_events / n_workers /
     * matches_event_filter are PGWT_EXEC_INDEX_NOT_INDEXED. */
    struct pgwt_execution *rows;
    int num_rows;

    /* 0 in version 1: see NOT INDEXED above. A caller needing counts must
     * recompute them; it must not treat these rows as complete. */
    int counts_indexed;

    enum pgwt_exec_index_refusal refused;

    /* Measured work, for the falsifiable prediction. rows_examined is the
     * number of index entries the prefilter let through to the retention
     * predicate; it is what replaces "events in window". */
    long chunks_total, chunks_scanned, rows_examined;
};

/* The reference's own row-retention predicate, verbatim (see HALF-OPEN
 * above for why it is inclusive at both ends). `in_progress` is the
 * window-relative value. */
int pgwt_exec_index_retains(uint64_t start_ns, uint64_t end_ns, int in_progress,
                            uint64_t from_ns, uint64_t to_ns);

/* Answer one window from the index. 0 on success, -1 on refusal (out->refused
 * says which, out->rows is NULL and out->num_rows is 0). */
int pgwt_exec_index_query(const struct pgwt_exec_index *idx,
                          uint64_t from_ns, uint64_t to_ns,
                          struct pgwt_exec_index_query_result *out);

void pgwt_exec_index_query_free(struct pgwt_exec_index_query_result *out);

#endif /* PGWT_EXEC_INDEX_H */
