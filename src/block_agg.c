/* block_agg.c — per-committed-block transition aggregate.
 *
 * The contract this implements is stated, clause by clause, in block_agg.h.
 * Every refusal path below names the clause it enforces. Nothing here guesses:
 * a question this module cannot answer comes back as a negative status or as
 * PGWT_BLOCK_DECODE, never as a zero.
 */
#include "block_agg.h"

#include "compute.h"        /* struct pgwt_filter */
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

/* FNV-1a over the on-wire fields of every decoded record, contributing or
 * not. `flags` is deliberately excluded: it is a reader-side annotation and
 * is never persisted (pg_wait_tracer.h), so including it would make the hash
 * depend on which annotator ran. */
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
