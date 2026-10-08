/* block_agg.h — per-committed-block transition aggregate (paint-latency Phase 1)
 *
 * WHAT THIS IS
 * ------------
 * Six detail commands load the full raw event window on every request, at
 * ~0.68 s per million records. `transitions` is one of them, and the
 * per-second summary records cannot answer it: a per-second total does not
 * record which event FOLLOWED which.
 *
 * A committed trace block is immutable, so the answer `transitions` needs can
 * be precomputed per block and then MERGED: a table keyed by
 * (old_event, new_event) with count + summed duration is additive, so merging
 * two blocks is adding two tables. A request merges the aggregates of the
 * blocks WHOLLY INSIDE the window and decodes raw events only for the partial
 * blocks at each edge. Prediction: O(events in window) -> O(blocks x distinct
 * pairs). See docs/PAINT_LATENCY_PLAN.md.
 *
 * ── THE v1 CONTRACT ──────────────────────────────────────────────────────
 * PGWT_BLOCK_AGG_VERSION records WHICH RULE the precomputed numbers were
 * written under. Every clause below is true as of the commit that ships this
 * header (the #315 lesson: never ship a version whose stated contract is not
 * yet enforced). Each clause names the function that enforces it.
 *
 * C1. TRANSITIONS blocks only. A SAMPLES block is REFUSED, never aggregated
 *     (pgwt_block_agg_build -> PGWT_BAGG_REFUSED_BLOCK_TYPE). Sampled point
 *     observations are not state transitions.
 * C2. Committed blocks only. The caller must assert the block is committed
 *     (immutable); an uncommitted block is REFUSED
 *     (pgwt_block_agg_build -> PGWT_BAGG_REFUSED_UNCOMMITTED). The open block
 *     of a current.trace still being appended to is not aggregatable.
 * C3. UNFILTERED only. The tables keep no pid / class / event / query_id
 *     dimension, so they cannot answer a filtered request. A caller with any
 *     filter set must recompute from raw; pgwt_block_agg_filter_supported()
 *     is the single predicate that says so, and it returns 0 for every
 *     non-empty filter.
 * C4. The pair table counts exactly the records pgwt_block_agg_record_counts()
 *     accepts: not SAMPLE-flagged, neither endpoint hidden, new_event is not
 *     PGWT_EVENT_EXIT, neither endpoint a marker. That predicate is the same
 *     set of rules pgwt_compute_transitions() applies, and
 *     tests/test_block_agg.c pins them against each other by differential.
 * C5. The node table counts the records pgwt_block_agg_node_counts() accepts:
 *     not SAMPLE-flagged, old_event neither hidden nor a marker. This is a
 *     WIDER set than C4 (it keeps a record whose new_event is EXIT or hidden),
 *     deliberately, because that is what the raw `transitions` response sums
 *     per node. It is also NARROWER than the raw response in exactly one way:
 *     the raw node pass walks SAMPLE records too, which add zero nanoseconds
 *     but do create a node entry for event id 0. See the OPEN QUESTION below.
 * C6. Durations are UNCLIPPED. A record is attributed in full to the block
 *     that contains it, exactly as pgwt_compute_transitions() does — it does
 *     no window clipping either. A transition is ONE record, so a pair can
 *     never be split across two blocks: it is counted ONCE, in the block
 *     holding the record, never twice and never lost. The interval the
 *     old_event occupied may well have begun in an earlier block, or before
 *     the window opened; the record is still counted whole. This is the
 *     straddle rule, and tests/test_block_agg.c §4 pins it.
 * C7. Window semantics are HALF-OPEN [from, to) throughout
 *     (pgwt_block_agg_in_window, pgwt_block_agg_plan). See #316, where an
 *     inclusive end bound double-counted a record.
 * C8. Absence is never an answer. No aggregate, a version mismatch, an
 *     identity mismatch, a filter, or an allocation failure all produce a
 *     REFUSAL (DECODE / negative status), never a zero. A caller that cannot
 *     see must fall back to raw.
 *
 * ── OPEN QUESTION for the wiring step (deliberately not decided here) ────
 * The raw `transitions` response in src/server.c builds its node totals from
 * ALL loaded records, including SAMPLE-flagged ones. A sample has
 * old_event == 0 and duration_ns == 0, so in a MIXED window it can create a
 * node "CPU*" with total_ms == 0 that this aggregate does not have. The
 * difference is bounded to that one zero-nanosecond node entry. The wiring
 * commit must either keep node presence for id 0 from the raw side, or
 * decide that a zero-duration node manufactured by a sample is a defect and
 * remove it from the raw side too. Nothing here guesses.
 *
 * ── Second open question — A BLOCKER for the wiring step ─────────────────
 * src/server.c's loader admits an exact record when
 * `ts >= from_m && ts <= to_m` — INCLUSIVE at the end, not half-open. That is
 * NOT the convention here and this module does not pretend to straddle both.
 *
 * The MERGE side would survive the difference on its own (MERGE needs
 * `last_timestamp_ns < to`, so every record it folds in has ts < to under
 * either reading). The SKIP side would not: a block with
 * `first_timestamp_ns == to` is SKIPped, and under an inclusive end bound the
 * raw path would still have counted its record at ts == to. So wiring this
 * module behind the current loader would LOSE those records.
 *
 * Therefore the wiring commit must reconcile the loader's exact-record test
 * to half-open `[from, to)` — the direction #316 already established — before
 * `transitions` reads from this module. tests/test_block_agg.c §6b pins the
 * divergence with a literal so it cannot be forgotten: under an end-inclusive
 * raw predicate the difference is exactly the records at ts == to, and the
 * test asserts that difference rather than tolerating it.
 */
#ifndef PGWT_BLOCK_AGG_H
#define PGWT_BLOCK_AGG_H

#include "event_writer.h"      /* enum pgwt_block_type */
#include "pg_wait_tracer.h"    /* struct pgwt_trace_event */

#include <stdint.h>

/* Declared in compute.h; only ever used here through a pointer, so this
 * header stays independent of the compute layer. */
struct pgwt_filter;

/* Version of the aggregate FORMAT AND RULES (the contract above). Bump on any
 * change to which records are counted or how they are attributed. */
#define PGWT_BLOCK_AGG_VERSION 1u

/* Every refusal has its own code so a caller (and a test) can tell WHY the
 * fast path declined. 0 is the only success value. */
enum pgwt_bagg_status {
    PGWT_BAGG_OK                  =  0,
    PGWT_BAGG_REFUSED_VERSION     = -1,  /* version mismatch */
    PGWT_BAGG_REFUSED_IDENTITY    = -2,  /* different trace, or rewritten block */
    PGWT_BAGG_REFUSED_DUPLICATE   = -3,  /* this block is already merged in */
    PGWT_BAGG_REFUSED_BLOCK_TYPE  = -4,  /* not a TRANSITIONS block (C1) */
    PGWT_BAGG_REFUSED_UNCOMMITTED = -5,  /* block not committed (C2) */
    PGWT_BAGG_REFUSED_NOMEM       = -6,  /* allocation failed — refuse, never short */
    PGWT_BAGG_REFUSED_MODE        = -7,  /* operation illegal for this aggregate */
    PGWT_BAGG_REFUSED_INVALID     = -8,  /* uninitialised / unresolvable identity */
};

/* Identity of the TRACE FILE a block came from. A rotation produces a new file
 * with a different start_time_ns/clock_offset_ns, so two blocks carrying the
 * same trace identity provably came from the same file. An all-zero identity
 * is "unresolvable" and is refused rather than treated as a match. */
struct pgwt_trace_identity {
    uint32_t trace_version;      /* pgwt_trace_file_header.version */
    uint32_t pg_version;         /* pgwt_trace_file_header.pg_version */
    uint64_t start_time_ns;      /* header start_time_ns (CLOCK_REALTIME) */
    uint64_t clock_offset_ns;    /* header clock_offset_ns (CLOCK_MONOTONIC) */
};

/* Identity of ONE block. Every field is HEADER-DERIVED: a caller can read all
 * of it without decompressing the block, which is the point —
 * pgwt_block_agg_matches() can revalidate a cached aggregate for the cost of a
 * block-header read.
 *
 * LIMIT, stated plainly: a block whose header fields all match but whose
 * payload differs is NOT detected by matches(). Committed blocks are
 * append-only and immutable, and a rotation changes the trace identity, so
 * that case means a corrupt or forged file. For a caller that has decoded the
 * block anyway, pgwt_block_agg_verify_payload() compares the stored
 * payload_hash. */
struct pgwt_block_identity {
    struct pgwt_trace_identity trace;
    uint32_t block_index;          /* index within the file */
    uint32_t num_events;           /* block header num_events, as written */
    uint64_t file_offset;          /* byte offset of the block in the file */
    uint64_t first_timestamp_ns;   /* block header bounds, monotonic. These */
    uint64_t last_timestamp_ns;    /* cover ALL records, contributing or not,
                                    * so using them for containment is
                                    * conservative: never narrower than the
                                    * contributing records' own extent. */
};

struct pgwt_block_agg_pair {
    uint32_t from_event;
    uint32_t to_event;
    uint64_t count;
    uint64_t total_ns;
};

struct pgwt_block_agg_node {
    uint32_t event_id;
    uint64_t count;
    uint64_t total_ns;
};

/* Key of a block already folded into an aggregate — the double-count guard. */
struct pgwt_block_agg_key {
    uint64_t start_time_ns;
    uint64_t clock_offset_ns;
    uint64_t file_offset;
    uint32_t block_index;
};

enum pgwt_bagg_mode {
    PGWT_BAGG_MODE_NONE   = 0,  /* not initialised — every op refuses */
    PGWT_BAGG_MODE_BLOCK  = 1,  /* built from exactly one committed block;
                                 * immutable afterwards */
    PGWT_BAGG_MODE_WINDOW = 2,  /* a request's accumulator: block aggregates
                                 * merge in, boundary records add in */
};

struct pgwt_block_agg {
    uint32_t version;
    enum pgwt_bagg_mode mode;

    /* MODE_BLOCK: the block's own identity, trace pinned.
     * MODE_WINDOW: trace left zero and `any_trace` set, because a window can
     * span a rotation; duplicate detection still keys on the trace, so block 0
     * of two different files never collide. */
    struct pgwt_block_identity id;
    int      any_trace;

    /* Time bounds: min/max of the header bounds of every block merged in,
     * widened by every record added through add_event(). Zero-length when no
     * block and no record has contributed (bounds_set == 0). */
    int      bounds_set;
    uint64_t first_timestamp_ns;
    uint64_t last_timestamp_ns;

    /* Declared totals. pair_total_ns is the sum of pairs[].total_ns and
     * node_total_ns the sum of nodes[].total_ns; they differ because C5 is a
     * wider predicate than C4, and that difference is itself asserted. */
    uint64_t total_transitions;
    uint64_t pair_total_ns;
    uint64_t node_records;
    uint64_t node_total_ns;

    /* FNV-1a over every decoded record of every block built (MODE_BLOCK) —
     * not over records added through add_event(). */
    uint64_t payload_hash;

    struct pgwt_block_agg_pair *pairs;   /* open-addressed, power-of-two */
    int pair_cap, n_pairs;
    struct pgwt_block_agg_node *nodes;   /* open-addressed, power-of-two */
    int node_cap, n_nodes;
    struct pgwt_block_agg_key  *keys;    /* flat, one per merged block */
    int key_cap, n_keys;
};

/* What to do with one block for a given window. Exactly one of the three for
 * every block — a partition, which is what makes double-counting at the seam
 * impossible rather than merely unlikely. */
enum pgwt_block_plan {
    PGWT_BLOCK_SKIP   = 0,  /* cannot overlap [from, to) */
    PGWT_BLOCK_MERGE  = 1,  /* wholly inside, aggregate present: merge it */
    PGWT_BLOCK_DECODE = 2,  /* boundary block, or no usable aggregate: decode */
};

/* ── Predicates (pure; the single definition of "counts") ───────────────── */

/* C4: does this record contribute a (from, to) pair? */
int pgwt_block_agg_record_counts(const struct pgwt_trace_event *ev);
/* C5: does this record contribute to its old_event's node total? */
int pgwt_block_agg_node_counts(const struct pgwt_trace_event *ev);
/* C7: half-open [from, to) on the record's end timestamp. */
int pgwt_block_agg_in_window(const struct pgwt_trace_event *ev,
                             uint64_t from_mono_ns, uint64_t to_mono_ns);
/* C3: can the aggregate answer a request carrying this filter? NULL = no
 * filter = yes. Any non-empty field = no. */
int pgwt_block_agg_filter_supported(const struct pgwt_filter *f);

/* Is this trace identity resolvable at all? An all-zero identity is not, and
 * must never compare equal to another (a gate that cannot see must refuse). */
int pgwt_trace_identity_resolvable(const struct pgwt_trace_identity *t);

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/* A request accumulator. Zeroes *a and sets MODE_WINDOW. */
void pgwt_block_agg_init_window(struct pgwt_block_agg *a);
void pgwt_block_agg_free(struct pgwt_block_agg *a);

/* Build the aggregate of ONE committed TRANSITIONS block from its decoded
 * records. Refuses per C1/C2 and on an unresolvable identity; on any refusal
 * *out is left freed and MODE_NONE, so a refused aggregate cannot be mistaken
 * for an empty one. An EMPTY block (count == 0) is a SUCCESS with zero pairs:
 * "this block contributes nothing" is a fact, and distinguishable from a
 * refusal by the return code. */
int pgwt_block_agg_build(struct pgwt_block_agg *out,
                         const struct pgwt_block_identity *id,
                         enum pgwt_block_type block_type, int committed,
                         const struct pgwt_trace_event *events, int count);

/* Fold a block aggregate into a window accumulator. Refuses on version
 * mismatch, on an unresolvable or (for a trace-pinned destination) mismatched
 * trace identity, on a destination that is not MODE_WINDOW, and on a block
 * already merged in (the double-count guard). Associative and commutative.
 *
 * ATOMICITY, stated exactly: every refusal EXCEPT PGWT_BAGG_REFUSED_NOMEM
 * leaves *dst byte-identical, because all of them are decided before a single
 * number moves. A NOMEM refusal can leave *dst PARTIALLY merged — there is no
 * rollback — so a caller that sees NOMEM must discard the accumulator and
 * recompute the request from raw. It must never read the partial numbers:
 * that would be a short answer wearing a plausible face. */
int pgwt_block_agg_merge(struct pgwt_block_agg *dst,
                         const struct pgwt_block_agg *src);

/* Add one boundary record to a window accumulator. The caller is responsible
 * for the window test (it owns the raw path's own predicate);
 * pgwt_block_agg_in_window() is the canonical one. */
int pgwt_block_agg_add_event(struct pgwt_block_agg *a,
                             const struct pgwt_trace_event *ev);

/* ── Validation ────────────────────────────────────────────────────────── */

/* Does a stored aggregate still describe this block as its header reads now?
 * Compares version, trace identity and every header-derived field. */
int pgwt_block_agg_matches(const struct pgwt_block_agg *a,
                           const struct pgwt_block_identity *now);

/* For a caller that has decoded the block anyway: does the stored
 * payload_hash still match these records? */
int pgwt_block_agg_verify_payload(const struct pgwt_block_agg *a,
                                  const struct pgwt_trace_event *events,
                                  int count);

/* The merge/decode/skip decision. `have_agg` is 0 when no aggregate exists or
 * it failed validation — then the answer is DECODE, never MERGE (C8). */
enum pgwt_block_plan pgwt_block_agg_plan(const struct pgwt_block_identity *id,
                                         int have_agg,
                                         uint64_t from_mono_ns,
                                         uint64_t to_mono_ns);

/* ── Readout ───────────────────────────────────────────────────────────── */

/* Point lookup. Returns 1 and fills count/total_ns when present, 0 when the
 * pair is absent. Absent is reported as ABSENT, not as zero. */
int pgwt_block_agg_lookup(const struct pgwt_block_agg *a,
                          uint32_t from_event, uint32_t to_event,
                          uint64_t *count, uint64_t *total_ns);
int pgwt_block_agg_node_lookup(const struct pgwt_block_agg *a,
                               uint32_t event_id,
                               uint64_t *count, uint64_t *total_ns);

/* Deterministic readout: count DESC, then from_event ASC, then to_event ASC —
 * a TOTAL order, so the bytes do not depend on merge order or on hash-table
 * layout. (pgwt_compute_transitions()' own comparator orders by count only,
 * which is not a total order; a bit-exact row-by-row comparison against it is
 * only meaningful once ties are broken the same way. Noted for the wiring
 * step.) Caller frees *out. */
int pgwt_block_agg_pairs_sorted(const struct pgwt_block_agg *a,
                                struct pgwt_block_agg_pair **out, int *n);
/* total_ns DESC, then event_id ASC. Caller frees *out. */
int pgwt_block_agg_nodes_sorted(const struct pgwt_block_agg *a,
                                struct pgwt_block_agg_node **out, int *n);

#endif /* PGWT_BLOCK_AGG_H */
