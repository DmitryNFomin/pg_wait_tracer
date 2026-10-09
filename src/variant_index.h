/* variant_index.h — per-execution STEP-SEQUENCE index (paint-latency 3b).
 *
 * WHY THIS EXISTS, AND WHY PHASE 3 COULD NOT DO IT
 * ------------------------------------------------
 * Phase 3's src/exec_index.c indexes execution BOUNDARIES. `variants` does
 * not want boundaries: handle_variants() calls pgwt_compute_variants(),
 * which builds a flow pattern out of the ordered SEQUENCE of wait steps
 * between an execution's start and end marker, compresses adjacent
 * repetitions into loops, hashes the compressed pattern and groups
 * executions by that hash. A start/end pair cannot produce that answer, so
 * `variants` needs its own index — this one.
 *
 * It stores, per execution, the REDUCED contribution that execution makes
 * to its variant: the compressed pattern, its hash, the execution's total
 * wait time, its query_id, its raw step count, and its per-pattern-step
 * duration/count accumulators. The expensive parts (the marker walk, the
 * 128-step sequence, loop detection, hashing) happen ONCE per committed
 * block. A request then replays only pgwt_compute_variants' second half —
 * hash-table accumulation over the executions in the window — so it costs
 * O(chunks + executions in window x steps per execution) instead of
 * O(events in window) plus a loop-detection pass per execution.
 *
 * PARTIAL-SEQUENCE CARRY-OVER is the whole difficulty. An execution whose
 * events are spread over blocks N..N+k must come out as ONE sequence, not
 * k+1 truncated ones (two different executions truncated at the same step
 * would hash to the same "variant" — the headline silent-wrong mode here).
 * The per-pid, per-phase partial sequence therefore lives in the index
 * builder across pgwt_variant_index_add_block() calls and is turned into a
 * row only when its closing marker arrives.
 *
 * THE CONTRACT (version 1) — read before using a single field
 * -----------------------------------------------------------
 * pgwt_variant_index_query(idx, from_ns, to_ns, f, max_variants, phase, out)
 * fills out->res so that it is BIT-EXACTLY equal, field for field and in
 * the same order, to the struct pgwt_variants_result that
 *
 *     pgwt_compute_variants(E, n, f, max_variants, phase, &res)
 *
 * produces, where E is the array src/server.c's handle_variants() actually
 * passes: every record of the indexed stream with
 * from_ns <= timestamp_ns <= to_ns, in stream order, and nothing else.
 *
 * SELECTION IS KEYED, NOT HALF-OPEN (plan rule 4 as corrected 2026-10-09).
 * The loader selects a record by its END timestamp, INCLUSIVE AT BOTH ENDS
 * (src/server.c:2848-2850). pgwt_compute_variants does not clip anything:
 * it sums each in-sequence record's own duration_ns. So an execution is in
 * the answer exactly when BOTH its markers are selected, i.e.
 *
 *     start_ns >= from_ns  &&  close_ns <= to_ns
 *
 * (pgwt_variant_index_selects(), written once, used by the query and
 * available to tests). Nothing else about the answer depends on the window:
 * every record strictly between two selected markers has a timestamp
 * between them and is therefore selected too, which is what makes one
 * stored row answer every window bit-exactly.
 *
 * The argument that an execution the oracle reports is exactly a row this
 * predicate selects, in the same order: the oracle pairs a closing marker
 * with the LAST opening marker before it with no closing marker in between,
 * and that pairing depends only on the marker subsequence for that
 * (pid, phase). Restricting the stream to a contiguous time range can only
 * drop markers at the ends. If the partner opening marker is in-window the
 * pairing is unchanged; if it is below from_ns the oracle has no per-pid
 * state at that closing marker and reports nothing — and this predicate
 * excludes the row, because start_ns < from_ns. Rows are appended at their
 * closing marker, so index order is the oracle's emission order.
 *
 * EMPTY FILTER ONLY, AND THAT IS NOT A NO-OP. pgwt_compute_variants applies
 * pgwt_filter_matches() to every in-sequence record, which rejects a record
 * whose duration_ns exceeds its own timestamp_ns EVEN FOR AN ALL-ZERO
 * FILTER. The index bakes in exactly those empty-filter semantics, so the
 * query REFUSES (REFUSE_FILTER) for a NULL filter (whose semantics differ:
 * no rejection at all) and for any filter with a class, event, pid or
 * query_id set. A filtered `variants` request falls back to raw — the
 * flex the plan's minimum-shippable section allows, not a guess.
 *
 * COVERAGE IS CONTAINMENT, NOT INTERSECTION. A window not fully contained
 * in [cover_from_ns, cover_to_ns] REFUSES (REFUSE_RANGE) rather than
 * answering for the part somebody indexed and silently dropping the rest.
 * Both bounds are public so a caller can see what it would have to decode.
 * Unlike Phase 3's row list, a variant table is NOT appendable by the
 * caller (the p95 sample order, the hash-table collision chains and the
 * query_id sets all depend on close order), so in version 1 a window
 * reaching past cover_to_ns falls back to raw wholesale. See the OPEN
 * QUESTION note at the bottom.
 *
 * ABSENCE NEVER READS AS ZERO, AND -1 IS TRUTHY IN C. Every refusal zeroes
 * out->res, then poisons res.num_variants and res.total_executions to
 * PGWT_VARIANT_INDEX_NOT_INDEXED (-1), sets res.failed = 1, and sets
 * out->sequences_indexed = 0. The poison alone is not the guard — -1 is
 * truthy, so a caller testing the value would sail past it; res.failed is
 * the boolean that forces the caller off this path, and handle_variants
 * already branches on it.
 *
 * ORDER IS LOAD-BEARING, TWICE. (1) The accumulation order decides the
 * hash table's collision chains, which pattern is stored for a hash, the
 * order of each variant's query_id set (top_query_id is query_ids[0]) and
 * which executions land in the bounded p95 sample. (2) avg_loop_n is a
 * DOUBLE sum, so IEEE non-associativity makes its last bits
 * order-dependent. Both are pinned by accumulating rows in closing-marker
 * stream order, and by the index latching REFUSE_GAP on a stream whose
 * timestamps decrease rather than silently reordering. Equal timestamps
 * keep feed order on both sides (the loader's sort is stable).
 *
 * STEP ORDER INSIDE A SEQUENCE IS SIGNIFICANT. [A, B] and [B, A] are two
 * variants, not one: hash_pattern folds the steps in order. The one
 * exception is deliberate and inherited from the oracle — adjacent
 * repetitions collapse into a loop step, so [A, A, A] and [A, A] are the
 * same pattern with different avg_loop_n. Pinned by test_variant_index.c.
 *
 * VERSION IS THE CONTRACT (#315). PGWT_VARIANT_INDEX_VERSION is stamped at
 * init and checked on every query. Everything above is true of version 1
 * as shipped and is pinned by tests/test_variant_index.c; bump the version
 * in the same commit that changes any of it.
 *
 * FALL BACK, NEVER GUESS. Every way the query can fail to establish the
 * answer returns a distinct enum pgwt_variant_index_refusal and produces no
 * variants. out->res.num_variants == 0 && out->res.total_executions == 0 &&
 * out->refused == PGWT_VARIANT_INDEX_OK is the only encoding of "this
 * window genuinely contains no executions".
 *
 * NOT A SHARED IMPLEMENTATION, DELIBERATELY. compress_exec(), hash_pattern()
 * and the per-step walk are static inside src/compute.c and three other
 * agents hold that file, so they are reimplemented here. The duplication is
 * not taken on faith: tests/test_variant_index.c drives both through
 * randomised and literal sequences and requires bit-exact agreement, so a
 * drift between the copies is a red test, not a wrong tab. Folding them
 * into one copy belongs to the wiring step that is allowed to touch
 * compute.c.
 *
 * OPEN QUESTION FOR THE WIRING STEP (not a defect of this module): the
 * common "ends at now" window reaches past the last committed block and so
 * REFUSES here. Serving it needs either a raw decode of the whole window
 * (no win) or a query that accepts the caller's raw-decoded tail records
 * and walks them through a COPY of the builder's carry state, appending
 * their rows after the indexed ones. The second is exact and cheap, but it
 * is an API addition with its own guards (the tail must provably cover
 * (cover_to_ns, to_ns] and start where the index stopped), so it is left
 * to the step that wires this in rather than shipped unexercised.
 */
#ifndef PGWT_VARIANT_INDEX_H
#define PGWT_VARIANT_INDEX_H

#include "compute.h"
#include "pg_wait_tracer.h"

#include <stdint.h>

#define PGWT_VARIANT_INDEX_VERSION 1

/* Poison written into every count the index could not establish. -1 is not
 * a plausible variant or execution count. It is NOT the guard on its own:
 * -1 is truthy in C, so res.failed / out->sequences_indexed are what force
 * a caller onto the raw path. */
#define PGWT_VARIANT_INDEX_NOT_INDEXED (-1)

/* The oracle's own limits, mirrored so the merge is bit-identical. */
#define PGWT_VARIANT_INDEX_HT_SIZE   4096
#define PGWT_VARIANT_INDEX_MAX_RAW   128     /* raw_exec.events[] capacity */
#define PGWT_VARIANT_INDEX_MAX_SAMPLES 10000 /* PGWT_VARIANT_MAX_SAMPLES */

enum pgwt_variant_index_refusal {
    PGWT_VARIANT_INDEX_OK = 0,
    PGWT_VARIANT_INDEX_REFUSE_NULL,         /* NULL index or NULL out */
    PGWT_VARIANT_INDEX_REFUSE_VERSION,      /* version mismatch */
    PGWT_VARIANT_INDEX_REFUSE_UNSEALED,     /* seal() not called */
    PGWT_VARIANT_INDEX_REFUSE_BUILD_FAILED, /* allocation failed building */
    PGWT_VARIANT_INDEX_REFUSE_EMPTY,        /* no block was ever added */
    PGWT_VARIANT_INDEX_REFUSE_RANGE,        /* window not contained in coverage */
    PGWT_VARIANT_INDEX_REFUSE_BAD_WINDOW,   /* from_ns > to_ns */
    PGWT_VARIANT_INDEX_REFUSE_GAP,          /* block feed skipped/reordered */
    PGWT_VARIANT_INDEX_REFUSE_ALLOC,        /* query-side allocation failure */
    PGWT_VARIANT_INDEX_REFUSE_FILTER,       /* NULL or non-empty filter */
    PGWT_VARIANT_INDEX_REFUSE_PHASE,        /* phase outside EXEC/PLAN */
};

const char *pgwt_variant_index_refusal_str(enum pgwt_variant_index_refusal r);

/* One pattern step's contribution from ONE execution. Stored in a shared
 * arena so an execution costs 24 bytes per step it actually has, not 32
 * steps' worth. total_ns/count are this execution's own accumulators for
 * that step; the query sums them per variant, in close order, with the same
 * integer additions the oracle performs. */
struct pgwt_variant_index_step {
    uint32_t event_id;
    int32_t  is_loop;
    int32_t  loop_len;
    int32_t  count;
    uint64_t total_ns;
};

/* One indexed execution, window-INDEPENDENT by construction: nothing here
 * is derived from a request's [from, to), which is what lets one row answer
 * every window. Rows exist only for executions whose closing marker was
 * actually seen; an execution still open at the end of the feed has no row,
 * exactly as pgwt_compute_variants reports nothing for one. */
struct pgwt_variant_index_entry {
    uint64_t start_ns;      /* the opening marker's timestamp */
    uint64_t close_ns;      /* the closing marker's timestamp */
    uint64_t query_id;      /* raw_exec.query_id at close */
    uint64_t total_ns;      /* sum of the sequence's duration_ns */
    uint64_t hash;          /* hash of the compressed pattern */
    uint32_t pid;
    int32_t  raw_len;       /* sequence length AFTER the CPU-only fill-in */
    int32_t  num_steps;     /* compressed pattern length */
    int32_t  step_off;      /* first of num_steps entries in idx->steps */
    uint8_t  phase;         /* enum pgwt_variant_phase */
};

/* One add_block() call: the prefilter unit. A chunk whose rows cannot
 * satisfy the selection predicate for a window is skipped without touching
 * them, which is where O(events) becomes O(executions in window). */
struct pgwt_variant_index_chunk {
    int      block_idx;
    int      first_row;
    int      n_rows;
    uint64_t block_first_ns, block_last_ns;
    uint64_t min_start_ns;  /* min start_ns over the chunk's rows */
    uint64_t max_close_ns;  /* max close_ns over the chunk's rows */
};

struct pgwt_variant_index {
    uint32_t version;
    int      sealed;
    int      failed;
    enum pgwt_variant_index_refusal build_refusal;

    struct pgwt_variant_index_entry *rows;
    int      n_rows, cap_rows;

    struct pgwt_variant_index_step *steps;
    int      n_steps, cap_steps;

    struct pgwt_variant_index_chunk *chunks;
    int      n_chunks, cap_chunks;

    /* Coverage: the union of the block bounds actually fed in. Public so a
     * caller can see what it would have to decode. Containment, not
     * intersection — see the header comment. */
    uint64_t cover_from_ns, cover_to_ns;
    int      next_block_idx;

    /* Global stream order across the whole feed. The oracle sees ONE
     * timestamp-ordered array, so a decreasing timestamp is a different
     * input, not a different index: it latches REFUSE_GAP. */
    uint64_t last_event_ns;
    int      have_last_event;

    /* Partial sequences thrown away by a second opening marker with no
     * closing marker between (the #222 shape). The oracle drops those
     * executions in every window; counting them is what lets a test prove
     * the shape occurred instead of assuming it did. */
    int      n_discarded;

    /* Per-pid, per-phase partial sequence carried across add_block(). THIS
     * IS the partial-sequence carry-over. */
    void    *pids;              /* struct vi_pid_carry *, opaque */
    int      n_pids, cap_pids;
    void    *pid_ix;            /* struct pgwt_pid_index *, opaque */
};

void pgwt_variant_index_init(struct pgwt_variant_index *idx);
void pgwt_variant_index_free(struct pgwt_variant_index *idx);

/* Feed one committed block's decoded events, in stream order.
 *
 * `block_idx` must be the caller's running 0-based counter; any other value
 * latches REFUSE_GAP, so a partial or reordered block range can never be
 * queried as if it were complete. `block_first_ns`/`block_last_ns` are the
 * block's own header bounds (NOT derived from `events`, so a block whose
 * records were all filtered out still extends coverage honestly) and must
 * be non-decreasing across calls. A record outside its block's bounds, or
 * below the previous record's timestamp, is a caller/file inconsistency and
 * latches REFUSE_GAP rather than being clamped.
 *
 * Returns 0 on success, -1 on any failure (the index is then permanently
 * un-queryable and the query reports why). */
int pgwt_variant_index_add_block(struct pgwt_variant_index *idx, int block_idx,
                                 uint64_t block_first_ns, uint64_t block_last_ns,
                                 const struct pgwt_trace_event *events,
                                 int count);

/* Finalise chunk prefilter bounds. Must be called once, after the last
 * add_block(). 0 on success, -1 on failure. */
int pgwt_variant_index_seal(struct pgwt_variant_index *idx);

/* Executions whose sequence was still open when the feed ended — the
 * carry-over that never closed, summed over both phases. -1 if the index is
 * unusable. Exposed so a caller and the test can see the number rather than
 * infer it. */
int pgwt_variant_index_open_at_end(const struct pgwt_variant_index *idx);

/* Partial sequences that were DISCARDED by a second opening marker with no
 * closing marker between (the #222 shape: two EXEC_STARTs in a row). The
 * oracle drops those executions silently; this counts them so the test can
 * prove the shape occurred rather than assume it. -1 if unusable. */
int pgwt_variant_index_discarded(const struct pgwt_variant_index *idx);

struct pgwt_variant_index_query_result {
    /* Bit-exactly what pgwt_compute_variants fills for the same window,
     * filter, max_variants and phase. res.variants is malloc'd; free the
     * whole thing with pgwt_variant_index_query_free(). */
    struct pgwt_variants_result res;

    /* 1 only when res is a complete answer. 0 on every refusal, paired with
     * the -1 poison in res.num_variants / res.total_executions and
     * res.failed = 1, because -1 is truthy and a value test alone is not a
     * guard. */
    int sequences_indexed;

    enum pgwt_variant_index_refusal refused;

    /* Measured work, for the falsifiable prediction. rows_examined is the
     * number of index entries the prefilter let through to the selection
     * predicate and steps_examined the pattern steps merged; together they
     * are what replaces "events in the window". */
    long chunks_total, chunks_scanned, rows_examined, rows_selected,
         steps_examined;
};

/* The selection predicate, written once: an execution is in the answer
 * exactly when BOTH its markers fall in the loader's inclusive window. */
int pgwt_variant_index_selects(uint64_t start_ns, uint64_t close_ns,
                               uint64_t from_ns, uint64_t to_ns);

/* 1 when `f` is the only filter this index can answer for: non-NULL and
 * with no class, event, pid or query_id set. See EMPTY FILTER ONLY above —
 * an all-zero filter is NOT a no-op, and NULL is a different oracle. */
int pgwt_variant_index_filter_ok(const struct pgwt_filter *f);

/* Answer one window from the index. 0 on success, -1 on refusal. */
int pgwt_variant_index_query(const struct pgwt_variant_index *idx,
                             uint64_t from_ns, uint64_t to_ns,
                             const struct pgwt_filter *f, int max_variants,
                             enum pgwt_variant_phase phase,
                             struct pgwt_variant_index_query_result *out);

void pgwt_variant_index_query_free(struct pgwt_variant_index_query_result *out);

#endif /* PGWT_VARIANT_INDEX_H */
