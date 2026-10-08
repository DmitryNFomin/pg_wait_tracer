/* block_agg.c — per-committed-block transition aggregate.
 *
 * The contract this implements is stated, clause by clause, in block_agg.h.
 * Every refusal path below names the clause it enforces. Nothing here guesses:
 * a question this module cannot answer comes back as a negative status or as
 * PGWT_BLOCK_DECODE, never as a zero.
 */
#include "block_agg.h"

#include "compute.h"        /* struct pgwt_filter */
#include "event_reader.h"   /* block headers + decode, for the window builder */
#include "idle_rule.h"      /* pgwt_is_hidden_event */
#include "test_alloc_fail.h"

#include <stdlib.h>
#include <string.h>

/* ── Predicates ────────────────────────────────────────────────────────── */

/* C4. Identical in effect to the record filter inside
 * pgwt_compute_transitions() (src/compute.c). tests/test_block_agg.c §1 pins
 * the two against each other record by record, so a divergence in either is
 * a red test rather than a silently different number. The request filter is
 * NOT applied here: C3 makes a filtered request fall back to raw entirely. */
int pgwt_block_agg_record_counts(const struct pgwt_trace_event *ev)
{
    if (!ev)
        return 0;
    if (ev->flags & PGWT_EVENT_FLAG_SAMPLE)
        return 0;
    /* The impossible-record refusal from pgwt_filter_matches() (src/compute.c
     * ~line 224): a record whose duration exceeds its own absolute END
     * timestamp cannot be true, and the raw path refuses it rather than
     * repairing it, at that one chokepoint, so that every compute path agrees
     * on the same bytes. The aggregate must refuse it too or it would count a
     * link the raw path does not — found by tests/test_block_agg.c §1 before
     * this module had it, which is why §1 exists.
     *
     * NOTE the asymmetry, faithful to the product: the raw `transitions` node
     * pass in src/server.c does NOT go through pgwt_filter_matches(), so such
     * a record still contributes node time there. pgwt_block_agg_node_counts()
     * therefore does NOT carry this refusal. On real traces timestamp_ns is
     * CLOCK_REALTIME-derived (~1.8e18), so neither branch can fire; the
     * aggregate mirrors the raw path anyway, because "cannot happen" is not a
     * reason for two paths to answer differently. */
    if (ev->duration_ns > ev->timestamp_ns)
        return 0;
    if (pgwt_is_hidden_event(ev->old_event) || pgwt_is_hidden_event(ev->new_event))
        return 0;
    if (ev->new_event == PGWT_EVENT_EXIT)
        return 0;
    if (PGWT_IS_MARKER(ev->old_event) || PGWT_IS_MARKER(ev->new_event))
        return 0;
    return 1;
}

/* C5. Wider than C4 on purpose: this is what the raw `transitions` response
 * sums per graph node (src/server.c handle_transitions' node pass), which
 * looks only at old_event. SAMPLE records are excluded — see the OPEN
 * QUESTION in block_agg.h. */
int pgwt_block_agg_node_counts(const struct pgwt_trace_event *ev)
{
    if (!ev)
        return 0;
    if (ev->flags & PGWT_EVENT_FLAG_SAMPLE)
        return 0;
    if (pgwt_is_hidden_event(ev->old_event) || PGWT_IS_MARKER(ev->old_event))
        return 0;
    return 1;
}

/* C7. Half-open on the record's END timestamp, which is the timestamp the
 * trace stores (duration_ns is time already spent in old_event). */
int pgwt_block_agg_in_window(const struct pgwt_trace_event *ev,
                             uint64_t from_mono_ns, uint64_t to_mono_ns)
{
    if (!ev)
        return 0;
    return ev->timestamp_ns >= from_mono_ns && ev->timestamp_ns < to_mono_ns;
}

/* C3. Every non-empty filter field disqualifies the aggregate. Written as an
 * explicit per-field test rather than a memcmp against a zeroed struct so
 * that adding a filter field without deciding about it is a compile-visible
 * omission rather than an accidental "supported". */
int pgwt_block_agg_filter_supported(const struct pgwt_filter *f)
{
    if (!f)
        return 1;                       /* no filter at all */
    if (f->class_name[0] != '\0')
        return 0;
    if (f->event_id != 0)
        return 0;
    if (f->pid != 0)
        return 0;
    if (f->query_id != 0)
        return 0;
    return 1;
}

int pgwt_trace_identity_resolvable(const struct pgwt_trace_identity *t)
{
    if (!t)
        return 0;
    /* A trace file always has a nonzero magic-checked version and a nonzero
     * creation wall clock. An all-zero (or version-zero) identity means the
     * caller could not read a header — unknown, which is never a match. */
    if (t->trace_version == 0)
        return 0;
    if (t->start_time_ns == 0)
        return 0;
    return 1;
}

static int trace_identity_equal(const struct pgwt_trace_identity *a,
                                const struct pgwt_trace_identity *b)
{
    if (!pgwt_trace_identity_resolvable(a) || !pgwt_trace_identity_resolvable(b))
        return 0;                       /* unknown never equals anything */
    return a->trace_version   == b->trace_version &&
           a->pg_version      == b->pg_version &&
           a->start_time_ns   == b->start_time_ns &&
           a->clock_offset_ns == b->clock_offset_ns;
}

/* ── Hash tables (open addressing, power-of-two, grow at 70%) ──────────── */

static uint32_t pair_hash(uint32_t from, uint32_t to)
{
    uint64_t h = ((uint64_t)from << 32) | to;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (uint32_t)h;
}

static uint32_t node_hash(uint32_t id)
{
    uint64_t h = id;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 29;
    return (uint32_t)h;
}

/* A slot is occupied iff count > 0. Every insert sets count >= 1, so there is
 * no tombstone case and no "present with count 0" ambiguity. */
static struct pgwt_block_agg_pair *
pair_slot(struct pgwt_block_agg_pair *tab, int cap, uint32_t from, uint32_t to)
{
    uint32_t mask = (uint32_t)cap - 1;
    uint32_t h = pair_hash(from, to) & mask;
    for (;;) {
        if (tab[h].count == 0)
            return &tab[h];
        if (tab[h].from_event == from && tab[h].to_event == to)
            return &tab[h];
        h = (h + 1) & mask;
    }
}

static struct pgwt_block_agg_node *
node_slot(struct pgwt_block_agg_node *tab, int cap, uint32_t id)
{
    uint32_t mask = (uint32_t)cap - 1;
    uint32_t h = node_hash(id) & mask;
    for (;;) {
        if (tab[h].count == 0)
            return &tab[h];
        if (tab[h].event_id == id)
            return &tab[h];
        h = (h + 1) & mask;
    }
}

static int pairs_grow(struct pgwt_block_agg *a)
{
    int ncap = a->pair_cap ? a->pair_cap * 2 : 64;
    if (ncap <= a->pair_cap)
        return PGWT_BAGG_REFUSED_NOMEM;
    struct pgwt_block_agg_pair *nt =
        pgwt_test_alloc_fail("block_agg_pairs_grow")
            ? NULL : calloc((size_t)ncap, sizeof(*nt));
    if (!nt)
        return PGWT_BAGG_REFUSED_NOMEM;
    for (int i = 0; i < a->pair_cap; i++) {
        if (a->pairs[i].count == 0)
            continue;
        *pair_slot(nt, ncap, a->pairs[i].from_event, a->pairs[i].to_event) =
            a->pairs[i];
    }
    free(a->pairs);
    a->pairs = nt;
    a->pair_cap = ncap;
    return PGWT_BAGG_OK;
}

static int nodes_grow(struct pgwt_block_agg *a)
{
    int ncap = a->node_cap ? a->node_cap * 2 : 64;
    if (ncap <= a->node_cap)
        return PGWT_BAGG_REFUSED_NOMEM;
    struct pgwt_block_agg_node *nt =
        pgwt_test_alloc_fail("block_agg_nodes_grow")
            ? NULL : calloc((size_t)ncap, sizeof(*nt));
    if (!nt)
        return PGWT_BAGG_REFUSED_NOMEM;
    for (int i = 0; i < a->node_cap; i++) {
        if (a->nodes[i].count == 0)
            continue;
        *node_slot(nt, ncap, a->nodes[i].event_id) = a->nodes[i];
    }
    free(a->nodes);
    a->nodes = nt;
    a->node_cap = ncap;
    return PGWT_BAGG_OK;
}

static int pair_add(struct pgwt_block_agg *a, uint32_t from, uint32_t to,
                    uint64_t count, uint64_t total_ns)
{
    if (count == 0)
        return PGWT_BAGG_OK;            /* nothing to record */
    if (a->pair_cap == 0 || (a->n_pairs + 1) * 10 >= a->pair_cap * 7) {
        int rc = pairs_grow(a);
        if (rc != PGWT_BAGG_OK)
            return rc;
    }
    struct pgwt_block_agg_pair *s = pair_slot(a->pairs, a->pair_cap, from, to);
    if (s->count == 0) {
        s->from_event = from;
        s->to_event = to;
        a->n_pairs++;
    }
    s->count += count;
    s->total_ns += total_ns;
    a->total_transitions += count;
    a->pair_total_ns += total_ns;
    return PGWT_BAGG_OK;
}

static int node_add(struct pgwt_block_agg *a, uint32_t id,
                    uint64_t count, uint64_t total_ns)
{
    if (count == 0)
        return PGWT_BAGG_OK;
    if (a->node_cap == 0 || (a->n_nodes + 1) * 10 >= a->node_cap * 7) {
        int rc = nodes_grow(a);
        if (rc != PGWT_BAGG_OK)
            return rc;
    }
    struct pgwt_block_agg_node *s = node_slot(a->nodes, a->node_cap, id);
    if (s->count == 0) {
        s->event_id = id;
        a->n_nodes++;
    }
    s->count += count;
    s->total_ns += total_ns;
    a->node_records += count;
    a->node_total_ns += total_ns;
    return PGWT_BAGG_OK;
}

/* ── Merged-block key set (the double-count guard) ─────────────────────── */

static int key_equal(const struct pgwt_block_agg_key *a,
                     const struct pgwt_block_agg_key *b)
{
    return a->start_time_ns   == b->start_time_ns &&
           a->clock_offset_ns == b->clock_offset_ns &&
           a->file_offset     == b->file_offset &&
           a->block_index     == b->block_index;
}

static void key_of(struct pgwt_block_agg_key *k,
                   const struct pgwt_block_identity *id)
{
    k->start_time_ns   = id->trace.start_time_ns;
    k->clock_offset_ns = id->trace.clock_offset_ns;
    k->file_offset     = id->file_offset;
    k->block_index     = id->block_index;
}

static int key_add(struct pgwt_block_agg *a, const struct pgwt_block_agg_key *k)
{
    for (int i = 0; i < a->n_keys; i++)
        if (key_equal(&a->keys[i], k))
            return PGWT_BAGG_REFUSED_DUPLICATE;
    if (a->n_keys == a->key_cap) {
        int ncap = a->key_cap ? a->key_cap * 2 : 16;
        struct pgwt_block_agg_key *nk =
            pgwt_test_alloc_fail("block_agg_keys_grow")
                ? NULL : realloc(a->keys, (size_t)ncap * sizeof(*nk));
        if (!nk)
            return PGWT_BAGG_REFUSED_NOMEM;
        a->keys = nk;
        a->key_cap = ncap;
    }
    a->keys[a->n_keys++] = *k;
    return PGWT_BAGG_OK;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

void pgwt_block_agg_init_window(struct pgwt_block_agg *a)
{
    if (!a)
        return;
    memset(a, 0, sizeof(*a));
    a->version = PGWT_BLOCK_AGG_VERSION;
    a->mode = PGWT_BAGG_MODE_WINDOW;
    a->any_trace = 1;
}

void pgwt_block_agg_free(struct pgwt_block_agg *a)
{
    if (!a)
        return;
    free(a->pairs);
    free(a->nodes);
    free(a->keys);
    memset(a, 0, sizeof(*a));          /* mode becomes NONE: every op refuses */
}

static void bounds_widen(struct pgwt_block_agg *a, uint64_t first, uint64_t last)
{
    if (!a->bounds_set) {
        a->bounds_set = 1;
        a->first_timestamp_ns = first;
        a->last_timestamp_ns = last;
        return;
    }
    if (first < a->first_timestamp_ns)
        a->first_timestamp_ns = first;
    if (last > a->last_timestamp_ns)
        a->last_timestamp_ns = last;
}

/* FNV-1a over THE FIELDS THIS AGGREGATE DEPENDS ON, for every decoded record,
 * contributing or not: timestamp_ns, pid, old_event, new_event, duration_ns,
 * query_id.
 *
 * Deliberately NOT every on-wire field. `cpu_ns` is on the wire (trace v3) and
 * is not hashed, because no number this module produces reads it — a block
 * that differed only in cpu_ns would still yield byte-identical pair and node
 * tables, so treating it as a mismatch would refuse a usable aggregate for no
 * gain. `flags` is excluded for a different reason: it is a reader-side
 * annotation, never persisted (pg_wait_tracer.h), so hashing it would make the
 * hash depend on which annotator ran. If a later version starts reading
 * cpu_ns, it must add it here AND bump PGWT_BLOCK_AGG_VERSION. */
static uint64_t payload_hash_of(const struct pgwt_trace_event *events, int count)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < count; i++) {
        uint64_t fields[6] = {
            events[i].timestamp_ns, events[i].pid, events[i].old_event,
            events[i].new_event, events[i].duration_ns, events[i].query_id,
        };
        for (int f = 0; f < 6; f++) {
            uint64_t v = fields[f];
            for (int b = 0; b < 8; b++) {
                h ^= (v >> (b * 8)) & 0xff;
                h *= 0x100000001b3ULL;
            }
        }
    }
    return h;
}

int pgwt_block_agg_build(struct pgwt_block_agg *out,
                         const struct pgwt_block_identity *id,
                         enum pgwt_block_type block_type, int committed,
                         const struct pgwt_trace_event *events, int count)
{
    if (!out)
        return PGWT_BAGG_REFUSED_INVALID;
    memset(out, 0, sizeof(*out));
    if (!id)
        return PGWT_BAGG_REFUSED_INVALID;
    /* C1: a SAMPLES block has no old_event column at all. */
    if (block_type != PGWT_BLOCK_TRANSITIONS)
        return PGWT_BAGG_REFUSED_BLOCK_TYPE;
    /* C2: an uncommitted block can still gain records. */
    if (!committed)
        return PGWT_BAGG_REFUSED_UNCOMMITTED;
    /* C8: an identity we cannot resolve can never be revalidated later, so it
     * must not become a cacheable aggregate. */
    if (!pgwt_trace_identity_resolvable(&id->trace))
        return PGWT_BAGG_REFUSED_INVALID;
    if (count < 0 || (count > 0 && !events))
        return PGWT_BAGG_REFUSED_INVALID;
    if (id->last_timestamp_ns < id->first_timestamp_ns)
        return PGWT_BAGG_REFUSED_INVALID;

    out->version = PGWT_BLOCK_AGG_VERSION;
    out->mode = PGWT_BAGG_MODE_BLOCK;
    out->id = *id;
    out->any_trace = 0;
    bounds_widen(out, id->first_timestamp_ns, id->last_timestamp_ns);

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        if (pgwt_block_agg_record_counts(ev)) {
            int rc = pair_add(out, ev->old_event, ev->new_event, 1,
                              ev->duration_ns);
            if (rc != PGWT_BAGG_OK) {
                pgwt_block_agg_free(out);
                return rc;              /* refuse whole, never partial */
            }
        }
        if (pgwt_block_agg_node_counts(ev)) {
            int rc = node_add(out, ev->old_event, 1, ev->duration_ns);
            if (rc != PGWT_BAGG_OK) {
                pgwt_block_agg_free(out);
                return rc;
            }
        }
    }
    out->payload_hash = payload_hash_of(events, count);
    return PGWT_BAGG_OK;
}

/* ATOMICITY (stated here as well as in the merge paragraph of block_agg.h,
 * because this is where a reader looks): a PGWT_BAGG_REFUSED_NOMEM return can
 * leave *a PARTIALLY updated — the pair table may have taken the record and
 * then the node table failed to grow. There is no rollback. A caller that
 * sees NOMEM must DISCARD the accumulator and recompute from raw; it must not
 * read the partial numbers, which would be a short answer wearing a plausible
 * face. Every other refusal leaves *a untouched. */
int pgwt_block_agg_add_event(struct pgwt_block_agg *a,
                             const struct pgwt_trace_event *ev)
{
    if (!a || a->mode != PGWT_BAGG_MODE_WINDOW)
        return PGWT_BAGG_REFUSED_MODE;
    if (a->version != PGWT_BLOCK_AGG_VERSION)
        return PGWT_BAGG_REFUSED_VERSION;
    if (!ev)
        return PGWT_BAGG_REFUSED_INVALID;
    if (pgwt_block_agg_record_counts(ev)) {
        int rc = pair_add(a, ev->old_event, ev->new_event, 1, ev->duration_ns);
        if (rc != PGWT_BAGG_OK)
            return rc;
    }
    if (pgwt_block_agg_node_counts(ev)) {
        int rc = node_add(a, ev->old_event, 1, ev->duration_ns);
        if (rc != PGWT_BAGG_OK)
            return rc;
    }
    if (pgwt_block_agg_record_counts(ev) || pgwt_block_agg_node_counts(ev))
        bounds_widen(a, ev->timestamp_ns, ev->timestamp_ns);
    return PGWT_BAGG_OK;
}

int pgwt_block_agg_add_events(struct pgwt_block_agg *a,
                              const struct pgwt_trace_event *events, int count,
                              uint64_t from_mono_ns, uint64_t to_mono_ns,
                              uint64_t *n_in_window)
{
    if (n_in_window)
        *n_in_window = 0;
    if (!a || a->mode != PGWT_BAGG_MODE_WINDOW)
        return PGWT_BAGG_REFUSED_MODE;
    if (count < 0 || (count > 0 && !events))
        return PGWT_BAGG_REFUSED_INVALID;
    for (int i = 0; i < count; i++) {
        if (!pgwt_block_agg_in_window(&events[i], from_mono_ns, to_mono_ns))
            continue;
        if (n_in_window)
            (*n_in_window)++;
        int rc = pgwt_block_agg_add_event(a, &events[i]);
        if (rc != PGWT_BAGG_OK)
            return rc;
    }
    return PGWT_BAGG_OK;
}

int pgwt_block_agg_merge(struct pgwt_block_agg *dst,
                         const struct pgwt_block_agg *src)
{
    if (!dst || !src)
        return PGWT_BAGG_REFUSED_INVALID;
    /* A block aggregate is immutable once built; only a window accumulator
     * absorbs. Without this a caller could fold blocks into a block and lose
     * the identity the cache revalidates against. */
    if (dst->mode != PGWT_BAGG_MODE_WINDOW)
        return PGWT_BAGG_REFUSED_MODE;
    if (src->mode != PGWT_BAGG_MODE_BLOCK)
        return PGWT_BAGG_REFUSED_MODE;
    /* C8 / #315: a version we did not write is not a version we can read. */
    if (dst->version != PGWT_BLOCK_AGG_VERSION ||
        src->version != PGWT_BLOCK_AGG_VERSION)
        return PGWT_BAGG_REFUSED_VERSION;
    if (!pgwt_trace_identity_resolvable(&src->id.trace))
        return PGWT_BAGG_REFUSED_IDENTITY;
    if (!dst->any_trace && !trace_identity_equal(&dst->id.trace, &src->id.trace))
        return PGWT_BAGG_REFUSED_IDENTITY;

    struct pgwt_block_agg_key k;
    key_of(&k, &src->id);
    /* Done BEFORE any number moves, so a refused duplicate leaves dst
     * untouched rather than half-added. */
    int rc = key_add(dst, &k);
    if (rc != PGWT_BAGG_OK)
        return rc;

    for (int i = 0; i < src->pair_cap; i++) {
        if (src->pairs[i].count == 0)
            continue;
        rc = pair_add(dst, src->pairs[i].from_event, src->pairs[i].to_event,
                      src->pairs[i].count, src->pairs[i].total_ns);
        if (rc != PGWT_BAGG_OK)
            return rc;
    }
    for (int i = 0; i < src->node_cap; i++) {
        if (src->nodes[i].count == 0)
            continue;
        rc = node_add(dst, src->nodes[i].event_id, src->nodes[i].count,
                      src->nodes[i].total_ns);
        if (rc != PGWT_BAGG_OK)
            return rc;
    }
    if (src->bounds_set)
        bounds_widen(dst, src->first_timestamp_ns, src->last_timestamp_ns);
    return PGWT_BAGG_OK;
}

/* ── Validation ────────────────────────────────────────────────────────── */

int pgwt_block_agg_matches(const struct pgwt_block_agg *a,
                           const struct pgwt_block_identity *now)
{
    if (!a || !now)
        return 0;
    if (a->mode != PGWT_BAGG_MODE_BLOCK)
        return 0;
    if (a->version != PGWT_BLOCK_AGG_VERSION)
        return 0;
    if (!trace_identity_equal(&a->id.trace, &now->trace))
        return 0;
    return a->id.block_index        == now->block_index &&
           a->id.num_events         == now->num_events &&
           a->id.file_offset        == now->file_offset &&
           a->id.first_timestamp_ns == now->first_timestamp_ns &&
           a->id.last_timestamp_ns  == now->last_timestamp_ns;
}

int pgwt_block_agg_verify_payload(const struct pgwt_block_agg *a,
                                  const struct pgwt_trace_event *events,
                                  int count)
{
    if (!a || a->mode != PGWT_BAGG_MODE_BLOCK)
        return 0;
    if (count < 0 || (count > 0 && !events))
        return 0;
    return a->payload_hash == payload_hash_of(events, count);
}

enum pgwt_block_plan pgwt_block_agg_plan(const struct pgwt_block_identity *id,
                                         int have_agg,
                                         uint64_t from_mono_ns,
                                         uint64_t to_mono_ns)
{
    /* Nothing to say about a block we cannot describe, and an empty or
     * inverted window selects nothing. Both are SKIP, which is safe: SKIP
     * contributes no records, so a wrong SKIP shows up as a missing record in
     * the cross-check, never as a double-counted one. */
    if (!id)
        return PGWT_BLOCK_SKIP;
    if (to_mono_ns <= from_mono_ns)
        return PGWT_BLOCK_SKIP;
    if (id->last_timestamp_ns < id->first_timestamp_ns)
        return PGWT_BLOCK_DECODE;      /* nonsense header: do not trust bounds */

    /* Half-open [from, to) against the block's header bounds (C7). */
    if (id->last_timestamp_ns < from_mono_ns)
        return PGWT_BLOCK_SKIP;
    if (id->first_timestamp_ns >= to_mono_ns)
        return PGWT_BLOCK_SKIP;

    /* C8: no usable aggregate means recompute, never approximate. */
    if (!have_agg)
        return PGWT_BLOCK_DECODE;

    /* Wholly inside. `last < to` (not `<=`) is deliberate and conservative:
     * a block ending exactly at `to` decodes. That costs one extra block and
     * makes the plan correct whether the caller's raw predicate treats the
     * end bound as inclusive or exclusive — every record in a MERGEd block
     * then has ts < to under either reading. */
    if (id->first_timestamp_ns >= from_mono_ns && id->last_timestamp_ns < to_mono_ns)
        return PGWT_BLOCK_MERGE;

    return PGWT_BLOCK_DECODE;
}

/* ── Readout ───────────────────────────────────────────────────────────── */

int pgwt_block_agg_lookup(const struct pgwt_block_agg *a,
                          uint32_t from_event, uint32_t to_event,
                          uint64_t *count, uint64_t *total_ns)
{
    if (!a || a->pair_cap == 0 || a->mode == PGWT_BAGG_MODE_NONE)
        return 0;
    const struct pgwt_block_agg_pair *s =
        pair_slot(a->pairs, a->pair_cap, from_event, to_event);
    if (s->count == 0)
        return 0;                       /* ABSENT, not zero */
    if (count) *count = s->count;
    if (total_ns) *total_ns = s->total_ns;
    return 1;
}

int pgwt_block_agg_node_lookup(const struct pgwt_block_agg *a,
                               uint32_t event_id,
                               uint64_t *count, uint64_t *total_ns)
{
    if (!a || a->node_cap == 0 || a->mode == PGWT_BAGG_MODE_NONE)
        return 0;
    const struct pgwt_block_agg_node *s =
        node_slot(a->nodes, a->node_cap, event_id);
    if (s->count == 0)
        return 0;
    if (count) *count = s->count;
    if (total_ns) *total_ns = s->total_ns;
    return 1;
}

static int cmp_pair_total(const void *va, const void *vb)
{
    const struct pgwt_block_agg_pair *a = va, *b = vb;
    if (a->count != b->count)
        return a->count > b->count ? -1 : 1;
    if (a->from_event != b->from_event)
        return a->from_event < b->from_event ? -1 : 1;
    if (a->to_event != b->to_event)
        return a->to_event < b->to_event ? -1 : 1;
    return 0;
}

static int cmp_node_total(const void *va, const void *vb)
{
    const struct pgwt_block_agg_node *a = va, *b = vb;
    if (a->total_ns != b->total_ns)
        return a->total_ns > b->total_ns ? -1 : 1;
    if (a->event_id != b->event_id)
        return a->event_id < b->event_id ? -1 : 1;
    return 0;
}

int pgwt_block_agg_pairs_sorted(const struct pgwt_block_agg *a,
                                struct pgwt_block_agg_pair **out, int *n)
{
    if (!a || !out || !n || a->mode == PGWT_BAGG_MODE_NONE)
        return PGWT_BAGG_REFUSED_INVALID;
    *out = NULL;
    *n = 0;
    if (a->n_pairs == 0)
        return PGWT_BAGG_OK;
    struct pgwt_block_agg_pair *arr =
        pgwt_test_alloc_fail("block_agg_pairs_sorted")
            ? NULL : calloc((size_t)a->n_pairs, sizeof(*arr));
    if (!arr)
        return PGWT_BAGG_REFUSED_NOMEM;
    int k = 0;
    for (int i = 0; i < a->pair_cap && k < a->n_pairs; i++)
        if (a->pairs[i].count != 0)
            arr[k++] = a->pairs[i];
    qsort(arr, (size_t)k, sizeof(*arr), cmp_pair_total);
    *out = arr;
    *n = k;
    return PGWT_BAGG_OK;
}

int pgwt_block_agg_nodes_sorted(const struct pgwt_block_agg *a,
                                struct pgwt_block_agg_node **out, int *n)
{
    if (!a || !out || !n || a->mode == PGWT_BAGG_MODE_NONE)
        return PGWT_BAGG_REFUSED_INVALID;
    *out = NULL;
    *n = 0;
    if (a->n_nodes == 0)
        return PGWT_BAGG_OK;
    struct pgwt_block_agg_node *arr =
        pgwt_test_alloc_fail("block_agg_nodes_sorted")
            ? NULL : calloc((size_t)a->n_nodes, sizeof(*arr));
    if (!arr)
        return PGWT_BAGG_REFUSED_NOMEM;
    int k = 0;
    for (int i = 0; i < a->node_cap && k < a->n_nodes; i++)
        if (a->nodes[i].count != 0)
            arr[k++] = a->nodes[i];
    qsort(arr, (size_t)k, sizeof(*arr), cmp_node_total);
    *out = arr;
    *n = k;
    return PGWT_BAGG_OK;
}

/* ── One trace file's contribution to a window ──────────────────────────── */

void pgwt_trace_identity_of_reader(const struct pgwt_event_reader *r,
                                   struct pgwt_trace_identity *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!r)
        return;
    out->trace_version   = r->header.version;
    out->pg_version      = r->header.pg_version;
    out->start_time_ns   = r->header.start_time_ns;
    out->clock_offset_ns = r->header.clock_offset_ns;
}

int pgwt_block_agg_window_from_reader(struct pgwt_event_reader *r,
                                      uint64_t from_mono_ns,
                                      uint64_t to_mono_ns,
                                      struct pgwt_block_agg *acc,
                                      pgwt_bagg_lookup_fn lookup,
                                      pgwt_bagg_store_fn store, void *ctx,
                                      uint64_t *exact_in_window,
                                      int *merged, int *decoded)
{
    if (!r || !acc || acc->mode != PGWT_BAGG_MODE_WINDOW)
        return PGWT_BAGG_REFUSED_MODE;
    if (to_mono_ns <= from_mono_ns)
        return PGWT_BAGG_OK;            /* selects nothing, and that is a fact */

    struct pgwt_trace_identity tr;
    pgwt_trace_identity_of_reader(r, &tr);
    /* A header we cannot resolve can never be revalidated later, so it must
     * not become the basis of a cached answer (C8). */
    if (!pgwt_trace_identity_resolvable(&tr))
        return PGWT_BAGG_REFUSED_IDENTITY;

    struct pgwt_trace_event *buf = NULL;
    int rc = PGWT_BAGG_OK;

    for (int b = 0; b < r->num_blocks; b++) {
        struct pgwt_block_info bi;
        if (pgwt_reader_block_info(r, b, &bi) != 0) {
            rc = PGWT_BAGG_REFUSED_INVALID;   /* cannot see => refuse */
            break;
        }

        int overlaps = bi.last_timestamp_ns >= from_mono_ns &&
                       bi.first_timestamp_ns < to_mono_ns;
        if (bi.block_type != PGWT_BLOCK_TRANSITIONS) {
            if (overlaps) {
                rc = PGWT_BAGG_REFUSED_BLOCK_TYPE;
                break;
            }
            continue;
        }

        struct pgwt_block_identity id = {
            .trace              = tr,
            .block_index        = (uint32_t)b,
            .num_events         = bi.num_events,
            .file_offset        = r->block_index[b].file_offset,
            .first_timestamp_ns = bi.first_timestamp_ns,
            .last_timestamp_ns  = bi.last_timestamp_ns,
        };

        const struct pgwt_block_agg *cached = lookup ? lookup(ctx, &id) : NULL;
        enum pgwt_block_plan plan =
            pgwt_block_agg_plan(&id, cached != NULL, from_mono_ns, to_mono_ns);

        if (plan == PGWT_BLOCK_SKIP)
            continue;

        if (plan == PGWT_BLOCK_MERGE) {
            rc = pgwt_block_agg_merge(acc, cached);
            if (rc != PGWT_BAGG_OK)
                break;
            if (merged) (*merged)++;
            if (exact_in_window) *exact_in_window += id.num_events;
            continue;
        }

        /* DECODE: a boundary block, or one with no usable aggregate. */
        if (!buf) {
            buf = calloc(PGWT_BLOCK_EVENTS, sizeof(*buf));
            if (!buf) {
                rc = PGWT_BAGG_REFUSED_NOMEM;
                break;
            }
        }
        int n = pgwt_reader_decode_block_info(r, b, buf, PGWT_BLOCK_EVENTS, &bi);
        if (n < 0) {
            rc = PGWT_BAGG_REFUSED_INVALID;
            break;
        }
        /* No header-vs-decode record-count check here, deliberately: it would
         * be unreachable. pgwt_reader_decode_block_info() takes its own record
         * count FROM the same block-header field this identity carries
         * (src/event_reader.c, `count = bh.num_events`), so n and
         * bi.num_events cannot disagree — a corrupt count makes the reader
         * refuse the block outright, which lands on the n < 0 branch above.
         * A guard that cannot fire is not protection, it is code that looks
         * like protection; the mutation driver found this one GREEN (M18) and
         * it was deleted rather than left in with no test behind it. */
        uint64_t admitted = 0;
        rc = pgwt_block_agg_add_events(acc, buf, n, from_mono_ns, to_mono_ns,
                                       &admitted);
        if (rc != PGWT_BAGG_OK)
            break;
        if (decoded) (*decoded)++;
        if (exact_in_window) *exact_in_window += admitted;

        /* Having paid for the decode, build the block's aggregate and offer it
         * to the cache so the NEXT window that contains this block whole can
         * merge it instead. This is what makes the phase a win rather than a
         * rearrangement: without reuse the fast path decodes exactly what the
         * raw path decodes. */
        if (store) {
            struct pgwt_block_agg fresh;
            if (pgwt_block_agg_build(&fresh, &id, bi.block_type, 1, buf, n)
                == PGWT_BAGG_OK) {
                store(ctx, &fresh);
                pgwt_block_agg_free(&fresh);   /* no-op once ownership moved */
            }
        }
    }

    free(buf);
    return rc;
}
