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

/* C7. INCLUSIVE at both ends, on the record's END timestamp — which is the
 * timestamp the trace stores (duration_ns is time already spent in
 * old_event). An event ending exactly at `to` has its whole interval inside
 * the window, so it belongs to the window; this mirrors the raw loader
 * (src/server.c, `ts < from_m || ts > to_m` -> skip) exactly. See C7 in
 * block_agg.h for why this is not #316 returning. */
int pgwt_block_agg_in_window(const struct pgwt_trace_event *ev,
                             uint64_t from_mono_ns, uint64_t to_mono_ns)
{
    if (!ev)
        return 0;
    return ev->timestamp_ns >= from_mono_ns && ev->timestamp_ns <= to_mono_ns;
}

/* C7, lifted to FILE granularity. Same rule, same reason: a file whose
 * earliest record ends exactly at `to_m` still has that record inside the
 * window, so the file CAN contribute and must not be skipped.
 *
 * This exists as a shared predicate because the per-file gate was written out
 * by hand at three sites and one of them drifted: the aggregate path used
 * `mono_first >= to_m` while the raw loader (src/server.c, `mono_first >
 * sample_to_m`) and the marker loader (`mono_first > to_m`) both used `>`.
 * The consequence was a silent-wrong answer — a file skipped WHOLE by the fast
 * path and admitted by raw, so `total` and one link's value came back short by
 * exactly those records, with no refusal and no fidelity change. That is
 * mutation M21 one layer up from where M21 was pinned: the suite caught the
 * off-by-one at BLOCK granularity and the identical error shipped at FILE
 * granularity. Hand-copied comparisons are how that happens, so the rule now
 * has one definition and tests point at it.
 *
 * `last < from_m` (not `<=`) is the mirror image: a file whose latest record
 * ends exactly at `from_m` has that record in the window too. */
int pgwt_block_agg_file_can_contribute(uint64_t mono_first, uint64_t mono_last,
                                       uint64_t from_mono_ns,
                                       uint64_t to_mono_ns)
{
    if (mono_first > to_mono_ns)
        return 0;
    if (mono_last < from_mono_ns)
        return 0;
    return 1;
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

/* C9. pgwt_compute_top_events()' own admission test, with a zeroed filter:
 *
 *   if (!pgwt_filter_matches(f, ev) || pgwt_is_hidden_event(ev->old_event))
 *       continue;
 *   if (ev->flags & PGWT_EVENT_FLAG_IO_WORKER)
 *       continue;
 *
 * expanded through pgwt_filter_matches() (src/compute.c:200) for f == {0}:
 * markers out, the impossible record (duration > its own end timestamp) out,
 * and the four filter fields all empty so none of them fires.
 *
 * A THIRD predicate, and the differences from C4/C5 are both ways round, so
 * neither can be substituted for it:
 *   - it KEEPS a record whose new_event is EXIT or hidden (C4 drops those);
 *   - it KEEPS SAMPLE records (C4 and C5 both drop them) — their time is DB
 *     time in the Events tab, only their LATENCY is refused (C12);
 *   - it DROPS IO_WORKER records (C4 and C5 keep them) — the T2 exclusion,
 *     because io_worker time would double-represent I/O already counted in
 *     the requesting backend's AioIoCompletion wait.
 *
 * tests/test_block_agg.c §10 pins this against pgwt_compute_top_events()
 * itself, record by record, so a divergence in either is a red test. */
int pgwt_block_agg_event_counts(const struct pgwt_trace_event *ev)
{
    if (!ev)
        return 0;
    if (PGWT_IS_MARKER(ev->old_event))
        return 0;
    if (ev->duration_ns > ev->timestamp_ns)
        return 0;
    if (pgwt_is_hidden_event(ev->old_event))
        return 0;
    if (ev->flags & PGWT_EVENT_FLAG_IO_WORKER)
        return 0;
    return 1;
}

/* C10. event_window_ns() (src/compute.c:37) clause for clause. The zero
 * checks are not defensive padding: `from_ns == 0` / `to_ns == 0` mean
 * "unbounded on that side" there, and a whole-capture request passes 0 for
 * both, so dropping them would change every unfiltered answer. */
uint64_t pgwt_block_agg_clip_ns(const struct pgwt_trace_event *ev,
                                uint64_t from_ns, uint64_t to_ns)
{
    if (!ev)
        return 0;
    /* The impossible record, refused rather than clamped — see the long note
     * in event_window_ns(). C9 already excludes it; this is the same refusal
     * at the same place the original has it, so the two cannot drift. */
    if (ev->duration_ns > ev->timestamp_ns)
        return 0;
    uint64_t start = ev->timestamp_ns - ev->duration_ns;
    uint64_t end   = ev->timestamp_ns;
    if (from_ns != 0 && start < from_ns) start = from_ns;
    if (to_ns   != 0 && end   > to_ns)   end   = to_ns;
    return end > start ? end - start : 0;
}

/* C12. The log2 latency ladder. Identical to compute.c's
 * compute_duration_to_bucket() and map_reader.c's pgwt_duration_to_bucket(),
 * both of which are unreachable from here (one is static, the other is not in
 * pgwt-server's link set). So this is a third copy, and a third copy is only
 * safe if something fails when it drifts: tests/test_block_agg.c §10c sweeps
 * every one of the 16 boundaries +-1 ns against edges it LEARNS from
 * pgwt_compute_top_events()' own reported percentiles, so a drift here or
 * there is a red test rather than two copies agreeing on one mistake. */
uint32_t pgwt_block_agg_duration_bucket(uint64_t ns)
{
    uint64_t us = ns / 1000;
    if (us < 1)     return 0;
    if (us < 2)     return 1;
    if (us < 4)     return 2;
    if (us < 8)     return 3;
    if (us < 16)    return 4;
    if (us < 32)    return 5;
    if (us < 64)    return 6;
    if (us < 128)   return 7;
    if (us < 256)   return 8;
    if (us < 512)   return 9;
    if (us < 1024)  return 10;
    if (us < 2048)  return 11;
    if (us < 4096)  return 12;
    if (us < 8192)  return 13;
    if (us < 16384) return 14;
    return 15;
}

/* C9/C13. The per-event table's key IS the event id, and a class index is a
 * pure function of it, so a class filter and an event filter are both
 * selections the caller can make on the readout. pid and query_id are not:
 * the table has no such dimension, and no post-hoc selection can invent one.
 *
 * Written as an explicit per-field test for the same reason as
 * pgwt_block_agg_filter_supported(): adding a filter field without deciding
 * about it must be a compile-visible omission, not an accidental "yes". */
int pgwt_block_agg_event_filter_supported(const struct pgwt_filter *f)
{
    if (!f)
        return 1;
    if (f->pid != 0)
        return 0;
    if (f->query_id != 0)
        return 0;
    /* class_name and event_id are supported: both select on the key. */
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

/* ── v2: the per-event table ───────────────────────────────────────────── */

static struct pgwt_block_agg_event *
ev_slot(struct pgwt_block_agg_event *tab, int cap, uint32_t id)
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

static int events_grow(struct pgwt_block_agg *a)
{
    int ncap = a->ev_cap ? a->ev_cap * 2 : 64;
    if (ncap <= a->ev_cap)
        return PGWT_BAGG_REFUSED_NOMEM;
    struct pgwt_block_agg_event *nt =
        pgwt_test_alloc_fail("block_agg_events_grow")
            ? NULL : calloc((size_t)ncap, sizeof(*nt));
    if (!nt)
        return PGWT_BAGG_REFUSED_NOMEM;
    for (int i = 0; i < a->ev_cap; i++) {
        if (a->events[i].count == 0)
            continue;
        *ev_slot(nt, ncap, a->events[i].event_id) = a->events[i];
    }
    free(a->events);
    a->events = nt;
    a->ev_cap = ncap;
    return PGWT_BAGG_OK;
}

/* Reserve the slot for `id`, growing if needed. *is_new says whether the slot
 * was empty, which is the one bit pgwt_compute_top_events()' SAMPLE_CONT rule
 * depends on (C14). */
static int ev_reserve(struct pgwt_block_agg *a, uint32_t id,
                      struct pgwt_block_agg_event **slot, int *is_new)
{
    if (a->ev_cap == 0 || (a->n_events + 1) * 10 >= a->ev_cap * 7) {
        int rc = events_grow(a);
        if (rc != PGWT_BAGG_OK)
            return rc;
    }
    struct pgwt_block_agg_event *s = ev_slot(a->events, a->ev_cap, id);
    *is_new = (s->count == 0);
    if (*is_new) {
        s->event_id = id;
        a->n_events++;
    }
    *slot = s;
    return PGWT_BAGG_OK;
}

static void ev_min_start_widen(struct pgwt_block_agg *a, uint64_t start)
{
    if (!a->ev_min_start_set) {
        a->ev_min_start_set = 1;
        a->ev_min_start_ns = start;
        return;
    }
    if (start < a->ev_min_start_ns)
        a->ev_min_start_ns = start;
}

/* Fold ONE record into the per-event table. `clip_ns` is the DB-time
 * contribution the caller already computed (unclipped for a MODE_BLOCK
 * aggregate, clipped to the declared window for a MODE_WINDOW one — C10).
 *
 * THE one place the top_events row rules live, so build() and add_event()
 * cannot drift. C9 is applied by the caller so the caller can also count the
 * record for its own bookkeeping; applying it here as well would hide a
 * caller that forgot. */
static int ev_observe(struct pgwt_block_agg *a,
                      const struct pgwt_trace_event *ev, uint64_t clip_ns)
{
    struct pgwt_block_agg_event *s;
    int is_new;
    int rc = ev_reserve(a, ev->old_event, &s, &is_new);
    if (rc != PGWT_BAGG_OK)
        return rc;

    /* C14: a SAMPLE_CONT fragment adds duration but is not a new physical
     * observation — unless it is the first record for this event, which is
     * pgwt_compute_top_events()' `|| ht[h].count == 0` exactly. Without that
     * second clause the slot would stay at count 0 and read as empty. */
    if (!(ev->flags & PGWT_EVENT_FLAG_SAMPLE_CONT) || is_new) {
        s->count++;
        a->ev_records++;
    }
    s->total_ns += clip_ns;
    a->ev_total_ns += clip_ns;

    /* C12: latency from exact records only (FID-3), on FULL durations. */
    if (!(ev->flags & PGWT_EVENT_FLAG_SAMPLE)) {
        s->exact_count++;
        s->exact_total_ns += ev->duration_ns;
        if (ev->duration_ns > s->max_ns)
            s->max_ns = ev->duration_ns;
        s->hist[pgwt_block_agg_duration_bucket(ev->duration_ns)]++;
    }

    /* C10: the earliest wait START folded in — the number that decides
     * whether this aggregate can be merged into a window without clipping.
     * C9 guarantees duration_ns <= timestamp_ns, so this cannot wrap. */
    ev_min_start_widen(a, ev->timestamp_ns - ev->duration_ns);
    return PGWT_BAGG_OK;
}

/* Fold a whole already-accumulated row (the merge path). */
static int ev_add_row(struct pgwt_block_agg *a,
                      const struct pgwt_block_agg_event *src)
{
    if (src->count == 0)
        return PGWT_BAGG_OK;
    struct pgwt_block_agg_event *s;
    int is_new;
    int rc = ev_reserve(a, src->event_id, &s, &is_new);
    if (rc != PGWT_BAGG_OK)
        return rc;
    s->count          += src->count;
    s->total_ns       += src->total_ns;
    s->exact_count    += src->exact_count;
    s->exact_total_ns += src->exact_total_ns;
    if (src->max_ns > s->max_ns)
        s->max_ns = src->max_ns;
    for (int b = 0; b < HISTOGRAM_BUCKETS; b++)
        s->hist[b] += src->hist[b];
    a->ev_records  += src->count;
    a->ev_total_ns += src->total_ns;
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
    free(a->events);
    memset(a, 0, sizeof(*a));          /* mode becomes NONE: every op refuses */
}

/* C11. The declaration that turns the per-event table on. */
int pgwt_block_agg_enable_events(struct pgwt_block_agg *a,
                                 uint64_t from_mono_ns, uint64_t to_mono_ns)
{
    if (!a)
        return PGWT_BAGG_REFUSED_INVALID;
    if (a->mode != PGWT_BAGG_MODE_WINDOW)
        return PGWT_BAGG_REFUSED_MODE;
    if (a->version != PGWT_BLOCK_AGG_VERSION)
        return PGWT_BAGG_REFUSED_VERSION;
    /* An inverted window would clip every record to zero and look like a
     * legitimately empty answer. Both ends nonzero and to < from is the only
     * inverted case; a zero end means unbounded there, not an endpoint. */
    if (from_mono_ns != 0 && to_mono_ns != 0 && to_mono_ns < from_mono_ns)
        return PGWT_BAGG_REFUSED_INVALID;
    /* Retro-fitting a window onto a half-built accumulator would clip the
     * records added after the call and not the ones added before it, which is
     * a silently short answer. Refuse instead. */
    if (a->n_keys != 0 || a->n_pairs != 0 || a->n_nodes != 0 ||
        a->n_events != 0 || a->bounds_set)
        return PGWT_BAGG_REFUSED_MODE;
    a->events_on = 1;
    a->ev_from_ns = from_mono_ns;
    a->ev_to_ns = to_mono_ns;
    return PGWT_BAGG_OK;
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
    /* C14. pgwt_compute_top_events()' physical-observation rule is
     * ORDER-DEPENDENT for continuation fragments: a SAMPLE_CONT record bumps
     * `count` iff it is the FIRST record seen for that event. A per-block
     * table cannot reproduce that under merging — block A holding the
     * fragment and block B holding its parent sample give count 2 merged in
     * either order, while a single pass over B then A gives 1.
     *
     * It cannot happen through the reader: `flags` is a reader-side
     * annotation, never persisted (pg_wait_tracer.h), and SAMPLE_CONT is set
     * only by the exact-wins merge long after decode, on records C1 has
     * already excluded (a SAMPLES block is refused outright). But build() is
     * public, and "cannot happen" is not a reason to let a silently
     * order-dependent count through. REFUSED, so the caller decodes — where
     * the single pass makes the rule exact. Pinned by §11 E12. */
    for (int i = 0; i < count; i++)
        if (events[i].flags & PGWT_EVENT_FLAG_SAMPLE_CONT)
            return PGWT_BAGG_REFUSED_BLOCK_TYPE;

    out->version = PGWT_BLOCK_AGG_VERSION;
    out->mode = PGWT_BAGG_MODE_BLOCK;
    out->id = *id;
    out->any_trace = 0;
    /* C10/C11: a block aggregate ALWAYS carries its per-event table, because
     * the table is window-independent once the durations are left unclipped —
     * that is what lets one cached block serve any window that contains it.
     * `events_on` is therefore about MODE_WINDOW accumulators only. */
    out->events_on = 1;
    out->ev_from_ns = 0;
    out->ev_to_ns = 0;
    bounds_widen(out, id->first_timestamp_ns, id->last_timestamp_ns);

    for (int i = 0; i < count; i++) {
        const struct pgwt_trace_event *ev = &events[i];
        /* C9: the per-event table, with UNCLIPPED durations (C10). */
        if (pgwt_block_agg_event_counts(ev)) {
            int rc = ev_observe(out, ev, ev->duration_ns);
            if (rc != PGWT_BAGG_OK) {
                pgwt_block_agg_free(out);
                return rc;              /* refuse whole, never partial */
            }
        }
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
    /* C9/C10: the per-event table, with the duration CLIPPED to the declared
     * window — the clip is what makes a decoded boundary record agree with
     * pgwt_compute_top_events() bit for bit. Skipped entirely when no window
     * was declared (C11), so a Phase-1 caller pays nothing and reads nothing. */
    if (a->events_on && pgwt_block_agg_event_counts(ev)) {
        int rc = ev_observe(a, ev,
                            pgwt_block_agg_clip_ns(ev, a->ev_from_ns,
                                                   a->ev_to_ns));
        if (rc != PGWT_BAGG_OK)
            return rc;
    }
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
    /* Bounds cover every record that contributed A NUMBER, which since v2
     * includes a C9-only record (a SAMPLE one, say: C9 keeps it, C4 and C5
     * both drop it). Leaving it out would make the bounds narrower than the
     * data they are supposed to describe. */
    if (pgwt_block_agg_record_counts(ev) || pgwt_block_agg_node_counts(ev) ||
        (a->events_on && pgwt_block_agg_event_counts(ev)))
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

    /* C10. The per-event CLIP pre-flight, decided here — before a single
     * number moves — so a REFUSED_CLIP leaves *dst byte-identical and
     * pgwt_block_agg_window_from_reader()'s downgrade to DECODE cannot
     * double-count what a half-applied merge had already folded in.
     *
     * Three separate conditions, and none of them is redundant:
     *   1. the source must HAVE a per-event table at all. A v2 block always
     *      does, but merge() is public, so a MODE_BLOCK aggregate built by a
     *      future caller with the table off must refuse, never contribute a
     *      silent zero to dst's event totals (C8);
     *   2. no record of the block may END after `to` — otherwise the raw path
     *      would not have selected it (C7) and the clip at `to` would fire;
     *   3. no record of the block may START before `from` — otherwise its
     *      unclipped duration overstates its in-window contribution, which is
     *      exactly the lock-pileup error event_window_ns() exists to prevent.
     *
     * Condition 3 is the one that actually fires on real data, and it fires
     * per BLOCK rather than per window, so one long wait crossing the
     * window's left edge costs one decoded block, not the whole fast path. */
    if (dst->events_on) {
        if (!src->events_on)
            return PGWT_BAGG_REFUSED_UNAVAILABLE;
        if (src->bounds_set) {
            if (dst->ev_to_ns != 0 &&
                src->last_timestamp_ns > dst->ev_to_ns)
                return PGWT_BAGG_REFUSED_CLIP;
            if (dst->ev_from_ns != 0 &&
                src->first_timestamp_ns < dst->ev_from_ns)
                return PGWT_BAGG_REFUSED_CLIP;
        }
        if (dst->ev_from_ns != 0 && src->ev_min_start_set &&
            src->ev_min_start_ns < dst->ev_from_ns)
            return PGWT_BAGG_REFUSED_CLIP;
    }

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
    /* C10: the block's per-event rows are UNCLIPPED, and the pre-flight above
     * proved no clipping is needed for this block in this window, so adding
     * them raw is exact — not an approximation that happens to be close. */
    if (dst->events_on) {
        for (int i = 0; i < src->ev_cap; i++) {
            if (src->events[i].count == 0)
                continue;
            rc = ev_add_row(dst, &src->events[i]);
            if (rc != PGWT_BAGG_OK)
                return rc;
        }
        if (src->ev_min_start_set)
            ev_min_start_widen(dst, src->ev_min_start_ns);
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
    if (to_mono_ns < from_mono_ns)
        return PGWT_BLOCK_SKIP;
    if (id->last_timestamp_ns < id->first_timestamp_ns)
        return PGWT_BLOCK_DECODE;      /* nonsense header: do not trust bounds */

    /* Inclusive-at-both-ends against the block's header bounds (C7): a block
     * is out only if it cannot hold a record in [from, to]. */
    if (id->last_timestamp_ns < from_mono_ns)
        return PGWT_BLOCK_SKIP;
    if (id->first_timestamp_ns > to_mono_ns)
        return PGWT_BLOCK_SKIP;

    /* C8: no usable aggregate means recompute, never approximate. */
    if (!have_agg)
        return PGWT_BLOCK_DECODE;

    /* Wholly inside, under the SAME bound the records are selected by: every
     * record of a block with first >= from and last <= to satisfies
     * ts in [from, to], which is what makes folding the whole aggregate
     * sound. */
    if (id->first_timestamp_ns >= from_mono_ns &&
        id->last_timestamp_ns <= to_mono_ns)
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

/* ── v2 readout: per-event and per-class ───────────────────────────────── */

int pgwt_block_agg_events_available(const struct pgwt_block_agg *a)
{
    if (!a || a->mode == PGWT_BAGG_MODE_NONE)
        return 0;
    if (a->version != PGWT_BLOCK_AGG_VERSION)
        return 0;
    return a->events_on ? 1 : 0;
}

int pgwt_block_agg_event_lookup(const struct pgwt_block_agg *a,
                                uint32_t event_id,
                                struct pgwt_block_agg_event *out)
{
    if (!pgwt_block_agg_events_available(a) || a->ev_cap == 0)
        return 0;
    const struct pgwt_block_agg_event *s =
        ev_slot(a->events, a->ev_cap, event_id);
    if (s->count == 0)
        return 0;                       /* ABSENT, not a zeroed row */
    if (out)
        *out = *s;
    return 1;
}

static int cmp_event_total(const void *va, const void *vb)
{
    const struct pgwt_block_agg_event *a = va, *b = vb;
    if (a->total_ns != b->total_ns)
        return a->total_ns > b->total_ns ? -1 : 1;
    if (a->event_id != b->event_id)
        return a->event_id < b->event_id ? -1 : 1;
    return 0;
}

int pgwt_block_agg_events_sorted(const struct pgwt_block_agg *a,
                                 struct pgwt_block_agg_event **out, int *n)
{
    if (!a || !out || !n)
        return PGWT_BAGG_REFUSED_INVALID;
    *out = NULL;
    *n = 0;
    /* C11: the table being off is a REFUSAL, not an empty list. An empty list
     * would be indistinguishable from "this window has no events", which is a
     * different and perfectly possible fact. */
    if (!pgwt_block_agg_events_available(a))
        return PGWT_BAGG_REFUSED_UNAVAILABLE;
    if (a->n_events == 0)
        return PGWT_BAGG_OK;
    struct pgwt_block_agg_event *arr =
        pgwt_test_alloc_fail("block_agg_events_sorted")
            ? NULL : calloc((size_t)a->n_events, sizeof(*arr));
    if (!arr)
        return PGWT_BAGG_REFUSED_NOMEM;
    int k = 0;
    for (int i = 0; i < a->ev_cap && k < a->n_events; i++)
        if (a->events[i].count != 0)
            arr[k++] = a->events[i];
    qsort(arr, (size_t)k, sizeof(*arr), cmp_event_total);
    *out = arr;
    *n = k;
    return PGWT_BAGG_OK;
}

static int cmp_class_total(const void *va, const void *vb)
{
    const struct pgwt_block_agg_class *a = va, *b = vb;
    if (a->total_ns != b->total_ns)
        return a->total_ns > b->total_ns ? -1 : 1;
    if (a->class_index != b->class_index)
        return a->class_index < b->class_index ? -1 : 1;
    return 0;
}

/* C13. Folded from the per-event table, never stored beside it. */
int pgwt_block_agg_classes_sorted(const struct pgwt_block_agg *a,
                                  struct pgwt_block_agg_class **out, int *n)
{
    if (!a || !out || !n)
        return PGWT_BAGG_REFUSED_INVALID;
    *out = NULL;
    *n = 0;
    if (!pgwt_block_agg_events_available(a))
        return PGWT_BAGG_REFUSED_UNAVAILABLE;
    if (a->n_events == 0)
        return PGWT_BAGG_OK;

    struct pgwt_block_agg_class acc[PGWT_NUM_CLASSES];
    for (int c = 0; c < PGWT_NUM_CLASSES; c++) {
        acc[c].class_index = c;
        acc[c].count = 0;
        acc[c].total_ns = 0;
    }
    int used = 0;
    for (int i = 0; i < a->ev_cap; i++) {
        if (a->events[i].count == 0)
            continue;
        int c = pgwt_wait_class_index(a->events[i].event_id);
        /* An out-of-range class index would silently corrupt the stack array;
         * refuse rather than clamp into UNKNOWN, because a clamp would report
         * somebody else's time under the wrong class name. */
        if (c < 0 || c >= PGWT_NUM_CLASSES)
            return PGWT_BAGG_REFUSED_INVALID;
        if (acc[c].count == 0 && acc[c].total_ns == 0)
            used++;
        acc[c].count += a->events[i].count;
        acc[c].total_ns += a->events[i].total_ns;
    }
    /* `used` counted first-touch, which misses a class whose only rows carry
     * count 0 — impossible, since a stored row always has count >= 1 — and
     * double-counts nothing. Recount honestly anyway: cheap, and it removes
     * the reader's need to trust that argument. */
    used = 0;
    for (int c = 0; c < PGWT_NUM_CLASSES; c++)
        if (acc[c].count != 0)
            used++;
    if (used == 0)
        return PGWT_BAGG_OK;

    struct pgwt_block_agg_class *arr =
        pgwt_test_alloc_fail("block_agg_classes_sorted")
            ? NULL : calloc((size_t)used, sizeof(*arr));
    if (!arr)
        return PGWT_BAGG_REFUSED_NOMEM;
    int k = 0;
    for (int c = 0; c < PGWT_NUM_CLASSES; c++)
        if (acc[c].count != 0)
            arr[k++] = acc[c];
    qsort(arr, (size_t)k, sizeof(*arr), cmp_class_total);
    *out = arr;
    *n = k;
    return PGWT_BAGG_OK;
}

int pgwt_block_agg_db_time_ns(const struct pgwt_block_agg *a, uint64_t *out)
{
    if (out)
        *out = 0;
    if (!a || !out)
        return PGWT_BAGG_REFUSED_INVALID;
    if (!pgwt_block_agg_events_available(a))
        return PGWT_BAGG_REFUSED_UNAVAILABLE;
    uint64_t t = 0;
    for (int i = 0; i < a->ev_cap; i++) {
        if (a->events[i].count == 0)
            continue;
        /* pgwt_compute_top_events(): idle-but-visible events appear in the
         * list but are NOT part of the DB-Time denominator. */
        if (pgwt_is_idle_event(a->events[i].event_id))
            continue;
        t += a->events[i].total_ns;
    }
    *out = t;
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
                                      pgwt_bagg_store_fn store,
                                      pgwt_bagg_decode_fn decode, void *ctx,
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

        /* C7 at BLOCK granularity, through the ONE shared predicate. This
         * used to be hand-written here as
         *   bi.last_timestamp_ns >= from_mono_ns &&
         *   bi.first_timestamp_ns <  to_mono_ns
         * — half-open at the `to` end while C7 is inclusive, i.e. the fourth
         * copy of the comparison pgwt_block_agg_file_can_contribute() exists
         * to prevent, and drifted in the same direction as the one that
         * already shipped wrong once. It was unreachable in Phase 1 only
         * because the server's bagg_window_eligible() refused any window a
         * SAMPLES block touched; Phase 2 is the work that lifts that refusal,
         * so a SAMPLES block whose first record ends exactly at `to` would
         * have stopped being REFUSED and started being silently SKIPPED —
         * raw labelling the window MIXED while the aggregate called it EXACT.
         * Pinned by tests/test_block_agg.c §9c. */
        int overlaps = pgwt_block_agg_file_can_contribute(
            bi.first_timestamp_ns, bi.last_timestamp_ns,
            from_mono_ns, to_mono_ns);
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
            /* C10: the block is wholly inside the window by END timestamp,
             * but one of its waits STARTED before `from`, so its unclipped
             * per-event total overstates the in-window contribution. merge()
             * decided that in its pre-flight, so *acc is byte-identical and
             * falling through to DECODE re-reads the block and clips each
             * record. Slower for that one block; exact. */
            if (rc == PGWT_BAGG_REFUSED_CLIP) {
                acc->clip_decoded_blocks++;
                plan = PGWT_BLOCK_DECODE;
                rc = PGWT_BAGG_OK;
            } else if (rc != PGWT_BAGG_OK) {
                break;
            } else {
                if (merged) (*merged)++;
                if (exact_in_window) *exact_in_window += id.num_events;
                continue;
            }
        }

        /* DECODE: a boundary block, or one with no usable aggregate. */
        const struct pgwt_trace_event *rec = NULL;
        int n;
        if (decode) {
            n = decode(ctx, r, b, &rec, &bi);
        } else {
            if (!buf) {
                buf = calloc(PGWT_BLOCK_EVENTS, sizeof(*buf));
                if (!buf) {
                    rc = PGWT_BAGG_REFUSED_NOMEM;
                    break;
                }
            }
            n = pgwt_reader_decode_block_info(r, b, buf, PGWT_BLOCK_EVENTS,
                                              &bi);
            rec = buf;
        }
        if (n < 0 || !rec) {
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
        rc = pgwt_block_agg_add_events(acc, rec, n, from_mono_ns, to_mono_ns,
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
            if (pgwt_block_agg_build(&fresh, &id, bi.block_type, 1, rec, n)
                == PGWT_BAGG_OK) {
                store(ctx, &fresh);
                pgwt_block_agg_free(&fresh);   /* no-op once ownership moved */
            }
        }
    }

    free(buf);
    return rc;
}
