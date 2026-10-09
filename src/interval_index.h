/* interval_index.h — per-pid wait-interval index (paint-latency plan, Phase 4).
 *
 * WHY THIS EXISTS
 * ---------------
 * `concurrency` is the one detail view that per-second summaries can never
 * reconstruct: peak is `max over (bucket, event) of |distinct pids|`, and
 * SETS UNION, THEY DO NOT ADD. A per-block "peak per bucket" is therefore
 * unmergeable — one pid waiting across blocks N and N+1 inside the same
 * bucket is one pid, not two. The only mergeable structure is the interval
 * list itself, which is what this module stores.
 *
 * HONEST SIZE OF THE WIN. This is a COMPACTION, not an algorithmic jump: a
 * 24-byte waits-only row instead of a 48-byte record for every event
 * (markers, on-CPU gaps and idle waits included), plus no per-request filter
 * pass and no raw decode. Expect roughly 2-6x on the load, NOT the O(blocks)
 * collapse Phases 1 and 2 get. The plan's "16-byte" figure described
 * compute.c's burst_entry, which carries no end timestamp; peaks need one,
 * so a row here is 24 bytes. Nothing in this file claims more than that.
 *
 * THE BOUNDARY RULE — the whole safety argument
 * ---------------------------------------------
 * Each interval is stored EXACTLY ONCE, in the block that holds its END
 * record, with its true `start_ns` UNCLIPPED. That works because
 * `pgwt_compute_concurrency` selects records by their END timestamp
 * (`ev->timestamp_ns`), inclusive at both window ends, and only then clips
 * the interval to the window when deciding which buckets it overlaps. A
 * wait starting in block N and ending in N+1 therefore sits in N+1's list
 * and still overlaps N's buckets, exactly as the raw path does today.
 *
 * CLIPPING `start_ns` TO THE BLOCK START IS THE CARDINAL ERROR. It lowers
 * peaks in earlier buckets and shifts burst onsets later, so bursts silently
 * vanish. There is no code path here that writes anything but the record's
 * own `timestamp_ns - duration_ns`.
 *
 * WINDOW SEMANTICS, RECONCILED TO THE LOADER (never the reverse)
 * -------------------------------------------------------------
 * SELECTION and CONTRIBUTION are separate steps and have different rules:
 *   - selection: `from_ns <= end_ns <= to_ns`, INCLUSIVE at both ends. That
 *     is src/server.c's own record predicate (`timestamp_ns < from_m ||
 *     timestamp_ns > to_m` -> skip) and concurrency_qualifies' window test.
 *     An event ending exactly at `to` lies wholly inside the window.
 *   - contribution: the interval is clipped to the window when buckets are
 *     assigned (`end_ns > from_ns` guard plus the b_lo/b_hi arithmetic in
 *     pgwt_compute_concurrency). This module does none of that itself.
 * A wait still OPEN at `to` has its end record after `to` (or no end record
 * at all) and is therefore NOT selected. That exclusion is the raw path's
 * existing behaviour; this phase reproduces it and does not "fix" it — a
 * semantic change hidden inside a performance phase is exactly what plan
 * rule 1 forbids. `out->excluded_open_past_to` counts it so the test can
 * prove the case was reached.
 *
 * THE ORACLE IS THE SHIPPING COMPUTATION, AND SO IS THE ANSWER
 * -----------------------------------------------------------
 * `pgwt_interval_index_query()` does NOT reimplement bucketing or burst
 * detection. It materialises the selected intervals back into a compact
 * `struct pgwt_trace_event` array and calls the real
 * `pgwt_compute_concurrency()`. So peak tie-breaks, the sliding burst pass,
 * the per-bucket burst choice, the pid display sample and every allocation
 * refusal are the shipped ones by construction, and the only thing this
 * module can get wrong is WHICH INTERVALS it hands over — which is precisely
 * what the unit test's differential checks.
 *
 * ONE RESTATEMENT, DECLARED. `concurrency_qualifies()` (src/compute.c, just
 * above pgwt_compute_concurrency) is `static inline` and this phase must not
 * touch compute.c, so its time-INDEPENDENT half is restated here as
 * `pgwt_interval_index_stores()`. The restatement CALLS the two shipping
 * helpers it is made of — `pgwt_filter_matches()` (the FID-4 chokepoint,
 * which also refuses markers and records whose duration exceeds their own
 * end timestamp) and `pgwt_is_idle_event()` — and restates only the literal
 * `old_event == 0` test. The time half is applied at query time. This is
 * declared rather than hidden; the test pins the equivalence by differential
 * against the real computation over the full record array.
 *
 * ORDER IS PART OF THE ANSWER. pgwt_compute_concurrency gives a bucket to
 * the FIRST record that REACHES a count ("strictly greater" in its peak
 * update), so `peak_event[]` depends on the order of the array it is given.
 * Rows are stored in fed stream order; a feed that is not non-decreasing in
 * `timestamp_ns`, a block_idx that is not the expected next one, and a row
 * array that is not non-decreasing in `end_ns` at seal() all REFUSE
 * (REFUSE_GAP) instead of being silently reordered.
 *
 * FALL BACK, NEVER GUESS. Every way the query can fail to establish the
 * answer returns a distinct `enum pgwt_interval_index_refusal` and produces
 * NO result (`have_result == 0`). In particular:
 *   - a FILTERED request refuses (REFUSE_FILTERED): version 1 indexes the
 *     unfiltered record set, and a class/event/pid/query filter changes
 *     which records qualify. Filtered `concurrency` stays on raw.
 *   - a window selecting ZERO intervals refuses (REFUSE_NO_INTERVALS). This
 *     is not timidity: pgwt_compute_concurrency returns a ZEROED result
 *     (num_buckets 0, no peak arrays) when its `count` is 0, but returns
 *     num_buckets zero-valued buckets when `count > 0` and merely nothing
 *     qualifies. The index holds only qualifying rows, so it cannot tell
 *     those two apart, and they serialise differently (empty `peaks` array
 *     vs N zero buckets). Refusing is the only answer that is not a guess;
 *     a window with no waits is also the cheapest possible raw load.
 *   - an empty index, an unsealed one, a wrong version, a build that hit an
 *     allocation failure, a gapped/reordered feed, an inverted window, a
 *     window not CONTAINED in coverage, num_buckets <= 0 and a query-side
 *     allocation failure each refuse with their own reason.
 * Coverage is CONTAINMENT, not intersection: a window reaching past the last
 * indexed block refuses rather than silently shortening itself. `cover_from_ns`
 * and `cover_to_ns` are public so the caller can clamp and decode the
 * remainder from raw instead.
 *
 * WIRING PRECONDITION (version 1). The index answers for the records it is
 * FED. src/server.c's loader applies record-level surgery of its own on
 * degraded windows — sample-interval splitting and the T2 phantom-EXIT drop
 * — which this module does not model. Wiring must therefore only consult the
 * index for windows where the loader performs no such surgery (handle_concurrency
 * already requires PGWT_REQ_EXACT fidelity), or bump the version. Stating
 * this is the #315 rule: never ship a version whose contract is not yet true.
 *
 * VERSION IS THE CONTRACT. PGWT_INTERVAL_INDEX_VERSION is stamped at init and
 * checked on every query and seal. Everything above is true of version 1 as
 * shipped and is pinned by tests/test_interval_index.c. Bump the version in
 * the same commit that changes any of it.
 */
#ifndef PGWT_INTERVAL_INDEX_H
#define PGWT_INTERVAL_INDEX_H

#include "compute.h"
#include "pg_wait_tracer.h"

#include <stdint.h>

#define PGWT_INTERVAL_INDEX_VERSION 1

enum pgwt_interval_index_refusal {
    PGWT_INTERVAL_INDEX_OK = 0,
    PGWT_INTERVAL_INDEX_REFUSE_NULL,          /* NULL index/filter/out */
    PGWT_INTERVAL_INDEX_REFUSE_VERSION,       /* version stamp mismatch */
    PGWT_INTERVAL_INDEX_REFUSE_UNSEALED,      /* seal() not called */
    PGWT_INTERVAL_INDEX_REFUSE_BUILD_FAILED,  /* allocation failed at build */
    PGWT_INTERVAL_INDEX_REFUSE_EMPTY,         /* no block was ever added */
    PGWT_INTERVAL_INDEX_REFUSE_RANGE,         /* window not contained in cover */
    PGWT_INTERVAL_INDEX_REFUSE_BAD_WINDOW,    /* from_ns >= to_ns */
    PGWT_INTERVAL_INDEX_REFUSE_BAD_BUCKETS,   /* num_buckets <= 0 */
    PGWT_INTERVAL_INDEX_REFUSE_GAP,           /* feed skipped/reordered */
    PGWT_INTERVAL_INDEX_REFUSE_ALLOC,         /* query-side allocation */
    PGWT_INTERVAL_INDEX_REFUSE_FILTERED,      /* filtered request: use raw */
    PGWT_INTERVAL_INDEX_REFUSE_NO_INTERVALS,  /* nothing selected: use raw */
    PGWT_INTERVAL_INDEX_REFUSE_COMPUTE,       /* compute_concurrency failed */
};

const char *pgwt_interval_index_refusal_str(enum pgwt_interval_index_refusal r);

/* One wait interval. 24 bytes. Window-INDEPENDENT by construction: nothing
 * here is derived from a request's [from, to), which is what lets one index
 * answer every window.
 *
 * start_ns is the record's own `timestamp_ns - duration_ns` — the TRUE start,
 * never clipped to the block, the coverage or a window. */
struct pgwt_wait_interval {
    uint64_t start_ns;
    uint64_t end_ns;        /* the record's timestamp_ns */
    uint32_t pid;
    uint32_t event_id;      /* the record's old_event (the wait being left) */
};

/* One add_block() call. The prefilter unit: a chunk whose selected-end range
 * [min_end_ns, max_end_ns] cannot overlap the request window is skipped
 * without touching its rows.
 *
 * Both bounds are over the chunk's own rows and are EXACT, because selection
 * is by end timestamp alone — unlike the executions index there is no
 * "still open at capture end" case to widen them with: an interval exists in
 * this index only because its END record was seen. */
struct pgwt_interval_index_chunk {
    int      block_idx;
    int      first_row, n_rows;
    uint64_t block_first_ns, block_last_ns;
    uint64_t min_end_ns, max_end_ns;
};

struct pgwt_interval_index {
    uint32_t version;
    int      sealed;
    int      failed;
    enum pgwt_interval_index_refusal build_refusal;

    struct pgwt_wait_interval *rows;
    int      n_rows, cap_rows;

    struct pgwt_interval_index_chunk *chunks;
    int      n_chunks, cap_chunks;

    /* The union of the block bounds actually fed in. A query window not
     * CONTAINED in it refuses. Public: the caller clamps to this and decodes
     * the remainder from raw. */
    uint64_t cover_from_ns, cover_to_ns;
    int      next_block_idx;    /* the only block_idx add_block will accept */

    uint64_t last_event_ns;
    int      have_last_event;

    /* BUILD LEDGER. Public so a test can assert the interesting shapes were
     * actually indexed rather than assumed. `n_cross_block` is the one that
     * matters: intervals that straddle an INTERNAL block boundary — stored
     * in a block that is not the first, having begun before that block
     * opened. Those are exactly the ones a clipped start_ns would corrupt,
     * and a feed of a single block drives the counter to 0 by construction
     * (which tests/test_interval_index.c's `oneblock` falsification mode
     * relies on). The first block is excluded on purpose: an interval
     * beginning before the coverage itself is a different class, counted
     * separately by n_start_before_cover. */
    long     n_intervals;
    long     n_cross_block;
    long     n_start_before_cover;
    long     n_records_fed;
    long     n_records_skipped;    /* fed records the predicate rejected */
};

void pgwt_interval_index_init(struct pgwt_interval_index *idx);
void pgwt_interval_index_free(struct pgwt_interval_index *idx);

/* The time-INDEPENDENT half of compute.c's concurrency_qualifies(). See
 * "ONE RESTATEMENT, DECLARED" above. `f` must be non-NULL; pass the
 * unfiltered filter used at build time. */
int pgwt_interval_index_stores(const struct pgwt_filter *f,
                               const struct pgwt_trace_event *ev);

/* Feed one committed block's decoded events, in stream order.
 *
 * `block_idx` must be the caller's running 0-based counter; anything else
 * latches REFUSE_GAP, so a partial or reordered block range can never be
 * queried as if it were complete. `block_first_ns`/`block_last_ns` are the
 * block's own header bounds (NOT derived from `events`, so a block whose
 * records were all non-waits still extends coverage honestly) and must be
 * non-decreasing across calls with block_first_ns <= block_last_ns.
 *
 * A record with timestamp_ns outside [block_first_ns, block_last_ns], or a
 * record whose timestamp_ns goes backwards relative to anything fed so far,
 * is a caller/file inconsistency and latches REFUSE_GAP rather than being
 * clamped or sorted.
 *
 * Returns 0 on success, -1 on any failure (the index is then permanently
 * un-queryable and the query reports why). */
int pgwt_interval_index_add_block(struct pgwt_interval_index *idx,
                                  int block_idx,
                                  uint64_t block_first_ns,
                                  uint64_t block_last_ns,
                                  const struct pgwt_trace_event *events,
                                  int count);

/* Finalise chunk prefilter bounds and verify the row array is non-decreasing
 * in end_ns (the invariant the query's binary search and the peak tie-break
 * both rest on). Must be called once, after the last add_block().
 * Returns 0 on success, -1 on failure. */
int pgwt_interval_index_seal(struct pgwt_interval_index *idx);

struct pgwt_interval_index_query_result {
    /* The SHIPPING computation's own result, produced by
     * pgwt_compute_concurrency over the selected intervals. Valid only when
     * have_result != 0; free with pgwt_interval_index_query_free(). */
    struct pgwt_concurrency_result result;
    int have_result;

    enum pgwt_interval_index_refusal refused;

    /* Measured work, for the falsifiable prediction, and the non-vacuity
     * ledger for the selection rules. */
    long chunks_total, chunks_scanned;
    long rows_examined;             /* rows the prefilter let through */
    long intervals_used;            /* rows handed to the computation */
    long excluded_end_before_from;  /* ended before the window opened */
    long excluded_open_past_to;     /* still open at `to`: raw excludes it */
    long bytes_materialised;        /* synthetic event array size */
};

/* Answer one window from the index.
 *
 * `f` must be the request's filter; a filtered request REFUSES. The burst
 * parameters are passed straight through to pgwt_compute_concurrency.
 *
 * 0 on success, -1 on refusal (out->refused says which, out->have_result is
 * 0 and the result struct is zeroed). */
int pgwt_interval_index_query(const struct pgwt_interval_index *idx,
                              const struct pgwt_filter *f,
                              uint64_t from_ns, uint64_t to_ns,
                              int num_buckets,
                              uint64_t burst_window_ns, int burst_threshold,
                              struct pgwt_interval_index_query_result *out);

void pgwt_interval_index_query_free(
        struct pgwt_interval_index_query_result *out);

#endif /* PGWT_INTERVAL_INDEX_H */
