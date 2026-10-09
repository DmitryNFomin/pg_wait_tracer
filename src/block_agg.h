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
 *     the raw node pass walks SAMPLE records too, and this module drops them.
 *
 *     CORRECTION, and it matters because the old wording here was used to
 *     justify the drop as cost-free: "SAMPLE records add zero nanoseconds" is
 *     true of the record AS DECODED (src/event_reader.c:338-341 zeroes
 *     duration_ns) but FALSE of the record the `transitions` handler receives.
 *     After the exact-wins merge each surviving sample fragment arrives as
 *     old_event == 0 with a POSITIVE duration_ns (src/server.c:2434). So in a
 *     MIXED window this drop removes the whole uncovered sampled contribution
 *     from the DFG's CPU node total — a real change to a number a user sees,
 *     not a no-op.
 *
 *     Whether that change is RIGHT is an open decision, NOT settled here: the
 *     links already exclude samples, so dropping them from nodes makes nodes
 *     and links consistent, which is an argument for it. It is reachable
 *     today, because the fast path declines MIXED windows, so master and this
 *     branch both answer them from raw — where they now disagree. Referred to
 *     the owner; no test pins a mixed-window node total, so this was decided
 *     by accident and must not stay that way. Do not turn this paragraph back
 *     into a justification.
 * C6. Durations are UNCLIPPED. A record is attributed in full to the block
 *     that contains it, exactly as pgwt_compute_transitions() does — it does
 *     no window clipping either. A transition is ONE record, so a pair can
 *     never be split across two blocks: it is counted ONCE, in the block
 *     holding the record, never twice and never lost. The interval the
 *     old_event occupied may well have begun in an earlier block, or before
 *     the window opened; the record is still counted whole. This is the
 *     straddle rule, and tests/test_block_agg.c §4 pins it.
 * C7. TIME is half-open; RECORD SELECTION is inclusive at both ends, and
 *     those are the same rule read from opposite ends of an interval.
 *
 *     A trace event's timestamp_ns is when the wait ENDED, so an event ending
 *     exactly at `to` has its whole interval inside the window — selecting it
 *     is correct, and dropping it throws away a fully in-window wait. The raw
 *     loader therefore selects `ts in [from, to]` (src/server.c) and then
 *     CLIPS each interval's contribution to the window (event_window_ns,
 *     src/compute.c). This module reproduces the selection; it does not clip,
 *     because pgwt_compute_transitions() does not either (C6).
 *
 *     So: pgwt_block_agg_in_window is `ts >= from && ts <= to`, plan() SKIPs
 *     iff `first > to`, and MERGEs iff `first >= from && last <= to`.
 *
 *     THIS IS NOT #316 RETURNING, and a future reader will assume it is. #316
 *     is the same rule on the other key: the summary reader keys seconds by
 *     their START, so a second at `to` is out. An earlier draft of this module
 *     read "half-open throughout" as applying to the END key too and made the
 *     loader match; that zeroed test_data_aas and test_data_categories
 *     (Total AAS 4.0 -> 0), because their fixtures' events end exactly at the
 *     window end. Those tests encode the correct behaviour.
 * C8. Absence is never an answer. No aggregate, a version mismatch, an
 *     identity mismatch, a filter, or an allocation failure all produce a
 *     REFUSAL (DECODE / negative status), never a zero. A caller that cannot
 *     see must fall back to raw.
 *
 * ── OPEN QUESTION, STILL OPEN AND NOW LARGER THAN IT LOOKED ──────────────
 * The raw `transitions` response in src/server.c builds its node totals from
 * ALL loaded records, including SAMPLE-flagged ones. This paragraph used to
 * say the difference was "bounded to that one zero-nanosecond node entry",
 * on the premise that a sample has duration_ns == 0. That premise is WRONG
 * for the records this handler sees: decode zeroes duration_ns
 * (src/event_reader.c:338-341), but the exact-wins merge then emits each
 * surviving sample fragment with a POSITIVE duration_ns
 * (src/server.c:2434). So the difference is not one empty node — it is the
 * entire uncovered sampled contribution to the CPU node total in a MIXED
 * window.
 *
 * The wiring commit routed the raw node pass through
 * pgwt_block_agg_add_events (src/server.c), which applies C5 and therefore
 * drops those fragments. That is a change to a user-visible number, made
 * without a test pinning it, i.e. decided by accident — exactly what this
 * section said must not happen. It is referred to the owner rather than
 * defended or reverted on an implementer's authority. Whichever way it goes,
 * it needs a test that pins a MIXED-window node total; there is none today,
 * and that absence is the real defect here.
 *
 * ── The one documented ASYMMETRY against the raw path ────────────────────
 * A window larger than load_max_events() is refused by the raw path with a
 * structured "window too large" error, because materialising it would exceed
 * the memory bound. The aggregate path never materialises the window, so it
 * ANSWERS. That asymmetry is the entire point of the phase and is deliberate;
 * the bound is unchanged for the other twelve detail commands, which still
 * refuse. Consequence: above that size there is no raw answer to compare
 * against, so the merge machinery's bit-exactness is pinned at sizes raw CAN
 * compute (tests/test_block_agg.c §6, §9) and the aggregate's own internal
 * consistency carries it above them.
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
    PGWT_BLOCK_SKIP   = 0,  /* cannot overlap [from, to] — INCLUSIVE, C7 */
    PGWT_BLOCK_MERGE  = 1,  /* wholly inside, aggregate present: merge it */
    PGWT_BLOCK_DECODE = 2,  /* boundary block, or no usable aggregate: decode */
};

/* ── Predicates (pure; the single definition of "counts") ───────────────── */

/* C4: does this record contribute a (from, to) pair? */
int pgwt_block_agg_record_counts(const struct pgwt_trace_event *ev);
/* C5: does this record contribute to its old_event's node total? */
int pgwt_block_agg_node_counts(const struct pgwt_trace_event *ev);
/* C7: INCLUSIVE at both ends, on the record's end timestamp. NOT half-open —
 * see C7 above for why, and do not "correct" it to `<`: that reading cost a
 * wiring round and zeroed test_data_aas (Total AAS 4.0 -> 0). */
int pgwt_block_agg_in_window(const struct pgwt_trace_event *ev,
                             uint64_t from_mono_ns, uint64_t to_mono_ns);
/* C7 at FILE granularity: can a file spanning [mono_first, mono_last]
 * contribute to [from, to]? The same inclusive rule, shared rather than
 * hand-copied, because the hand-copied version drifted to `>=` and produced a
 * silently short answer. See the body in block_agg.c. */
int pgwt_block_agg_file_can_contribute(uint64_t mono_first, uint64_t mono_last,
                                       uint64_t from_mono_ns,
                                       uint64_t to_mono_ns);
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
 * pgwt_block_agg_in_window() is the canonical one.
 *
 * ATOMICITY, same rule as merge(): a PGWT_BAGG_REFUSED_NOMEM return can leave
 * *a PARTIALLY updated (the pair table may have taken the record while the
 * node table could not grow). There is no rollback. A caller that sees NOMEM
 * must discard the accumulator and recompute from raw, never read its
 * numbers. Every other refusal leaves *a untouched. */
int pgwt_block_agg_add_event(struct pgwt_block_agg *a,
                             const struct pgwt_trace_event *ev);

/* Accumulate every record of `events` whose timestamp is in [from, to] —
 * INCLUSIVE at both ends, C7 — into a window accumulator. THE one raw-side
 * accumulator: the raw `transitions` response and tests/test_block_agg.c's
 * oracle comparison both go through it, so the per-node numbers the product
 * emits cannot drift from the numbers the gate compares.
 *
 * CORRECTION: this used to claim "the aggregate-vs-raw cross-check" goes
 * through it too. It does not. tests/test_agg_raw_crosscheck.c is a different
 * gate — per-second SUMMARY records vs raw — and it does not link block_agg.c
 * at all (see its recipe in tests/Makefile). Naming an unrelated test as a
 * guard here would let a reader believe this function is covered by a gate
 * that never calls it.
 *
 * Pass from = 0, to = UINT64_MAX for "every record given".
 *
 * `*n_in_window` (optional) receives the number of records admitted by the
 * window test REGARDLESS of the counting predicates — that is the caller's
 * "did any exact record actually land in this window" signal, which is what
 * the fidelity indicator is derived from, and is NOT the same as
 * total_transitions. Returns 0 or a negative pgwt_bagg_status. */
int pgwt_block_agg_add_events(struct pgwt_block_agg *a,
                              const struct pgwt_trace_event *events, int count,
                              uint64_t from_mono_ns, uint64_t to_mono_ns,
                              uint64_t *n_in_window);

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

/* ── One trace file's contribution to a window ──────────────────────────── */

struct pgwt_event_reader;   /* event_reader.h */
struct pgwt_block_info;     /* event_reader.h */

/* Cache hooks. The POLICY (size caps, eviction, how long an entry lives) is
 * the caller's; the DECISION of whether an entry may be used is not — it is
 * pgwt_block_agg_matches(), which the lookup implementation must apply.
 *
 * lookup: return a usable aggregate for exactly this block identity, or NULL.
 *         NULL is a complete answer ("no aggregate") and makes the block
 *         DECODE; it must never return an entry it could not revalidate.
 * store:  offered a freshly built aggregate; may take ownership (and must then
 *         zero *agg) or decline (leave it alone — the caller frees it).
 *         Declining costs speed, never correctness.
 * decode: hand back block `block_idx`'s records. NULL means "decode straight
 *         from the reader", which is what a test or a one-shot caller wants.
 *         The server supplies one so a boundary block of current.trace is read
 *         through the SAME decoded-block cache the raw loader uses (#283)
 *         rather than re-decompressed — which also keeps that cache's
 *         served/decoded counters meaningful for a request this path
 *         answered. Returns the record count, or -1 to refuse. The records
 *         must stay valid until the next call. */
typedef const struct pgwt_block_agg *(*pgwt_bagg_lookup_fn)(
    void *ctx, const struct pgwt_block_identity *id);
typedef void (*pgwt_bagg_store_fn)(void *ctx, struct pgwt_block_agg *agg);
typedef int (*pgwt_bagg_decode_fn)(void *ctx, struct pgwt_event_reader *r,
                                   int block_idx,
                                   const struct pgwt_trace_event **out,
                                   struct pgwt_block_info *bi);

/* Fold one trace file's blocks into the window accumulator `acc`, merging the
 * blocks wholly inside [from_mono_ns, to_mono_ns] -- INCLUSIVE at both ends
 * (C7; do not "correct" the bracket) -- and decoding only the
 * partial ones at the edges.
 *
 * This is THE iteration both the shipped `transitions` handler and
 * tests/test_block_agg.c §9 drive, on purpose: the plan/merge/decode sequence
 * is where a seam double-count or a lost edge record would live, so the gate
 * must exercise the same code the server runs, not a restatement of it.
 *
 * `*exact_in_window` is incremented by the number of records admitted by the
 * window test regardless of the counting predicates — the caller's fidelity
 * signal (see pgwt_block_agg_add_events). `*merged`/`*decoded` count blocks,
 * for callers that want to declare how an answer was produced. All three are
 * optional.
 *
 */
int pgwt_block_agg_window_from_reader(struct pgwt_event_reader *r,
                                      uint64_t from_mono_ns,
                                      uint64_t to_mono_ns,
                                      struct pgwt_block_agg *acc,
                                      pgwt_bagg_lookup_fn lookup,
                                      pgwt_bagg_store_fn store,
                                      pgwt_bagg_decode_fn decode, void *ctx,
                                      uint64_t *exact_in_window,
                                      int *merged, int *decoded);

/* The trace identity of an open reader, from its file header. */
void pgwt_trace_identity_of_reader(const struct pgwt_event_reader *r,
                                   struct pgwt_trace_identity *out);

#endif /* PGWT_BLOCK_AGG_H */
