#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Production merge feed and qualification-only append sink. */
#include "merge_capture.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct retained {
    ma_envelope envelope;
    unsigned char bytes[272];
    struct retained *next;
};
struct arrival {
    uint64_t source, position, at;
    size_t prev, next;
};
struct pgwt_merge_capture {
    struct pgwt_capture_dump *dump;
    int dump_fd;
    ma *adapter;
    pgwt_merge *core;
    struct pgwt_capture_config config;
    ma_lifetime *lifetimes;
    ma_binding *bindings;
    size_t *lifetime_index, *binding_index;
    const ma_envelope **held;
    struct retained **heads, **tails, *storage, *free;
    size_t lifetime_count, binding_count, source_count, retained_count;
    size_t staged;
    uint64_t local_writer_fail, max_held, last_checkpoint_at;
    struct arrival *arrivals;
    size_t arrival_slots, oldest, newest, arrival_free;
    size_t *arrival_buckets;
    pgwt_merge_observation observation;
    bool finished, serviced;
    uint64_t last_service_at;
    /* Measurements only. Ages in PGWT_CAPTURE_AGE_BUCKET_NS buckets; the
     * last bucket collects everything at or beyond the range. */
    uint64_t released_data, released_text, frontiers, voids;
    uint64_t held_max, event_max;
    uint32_t *held_hist, *event_hist;
};
#define PGWT_CAPTURE_AGE_BUCKET_NS UINT64_C(100000)
#define PGWT_CAPTURE_AGE_BUCKETS 20000U /* 2 s */
static void age_add(uint32_t *hist, uint64_t *max, uint64_t age)
{
    if (age > *max) *max = age;
    uint64_t bucket = age / PGWT_CAPTURE_AGE_BUCKET_NS;
    if (bucket > PGWT_CAPTURE_AGE_BUCKETS) bucket = PGWT_CAPTURE_AGE_BUCKETS;
    if (hist && hist[bucket] != UINT32_MAX) ++hist[bucket];
}
/* Upper edge of the bucket holding the q-th record (q in 1/10000). */
static uint64_t age_quantile(const uint32_t *hist, unsigned q)
{
    uint64_t total = 0, seen = 0;
    if (!hist) return 0;
    for (size_t i = 0; i <= PGWT_CAPTURE_AGE_BUCKETS; ++i) total += hist[i];
    if (!total) return 0;
    uint64_t rank = (total * q + 9999) / 10000;
    for (size_t i = 0; i <= PGWT_CAPTURE_AGE_BUCKETS; ++i) {
        seen += hist[i];
        if (seen >= rank) return (i + 1) * PGWT_CAPTURE_AGE_BUCKET_NS;
    }
    return (PGWT_CAPTURE_AGE_BUCKETS + 1) * PGWT_CAPTURE_AGE_BUCKET_NS;
}
static uint64_t get_le(const unsigned char *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static size_t arrival_hash(const struct pgwt_merge_capture *c, size_t source, uint64_t position)
{
    return (size_t)((position * UINT64_C(11400714819323198485) ^ (uint64_t)source) % c->arrival_slots);
}
static size_t arrival_slot(const struct pgwt_merge_capture *c, size_t source, uint64_t position)
{
    size_t i = arrival_hash(c, source, position);
    while (c->arrival_buckets[i]) {
        const struct arrival *a = &c->arrivals[c->arrival_buckets[i] - 1];
        if (a->source == source && a->position == position) break;
        i = (i + 1) % c->arrival_slots;
    }
    return i;
}
static void held_age(struct pgwt_merge_capture *c, uint64_t now)
{
    if (c->oldest != SIZE_MAX && now >= c->arrivals[c->oldest].at &&
        now - c->arrivals[c->oldest].at > c->max_held)
        c->max_held = now - c->arrivals[c->oldest].at;
}
static void arrival_reset(struct pgwt_merge_capture *c)
{
    memset(c->arrival_buckets, 0, c->arrival_slots * sizeof(*c->arrival_buckets));
    for (size_t i = 0; i < c->config.capacity; ++i)
        c->arrivals[i].next = i + 1 == c->config.capacity ? SIZE_MAX : i + 1;
    c->arrival_free = 0; c->oldest = c->newest = SIZE_MAX;
}
static void arrival_remove(struct pgwt_merge_capture *c, const pgwt_merge_record *r, uint64_t now)
{
    size_t bucket = arrival_slot(c, r->source, r->o), index = c->arrival_buckets[bucket];
    if (!index) return;
    held_age(c, now);
    struct arrival *a = &c->arrivals[index - 1];
    /* This record's own age from wire consumption to publication. */
    if (now >= a->at) age_add(c->held_hist, &c->held_max, now - a->at);
    if (a->prev == SIZE_MAX) c->oldest = a->next;
    else c->arrivals[a->prev].next = a->next;
    if (a->next == SIZE_MAX) c->newest = a->prev;
    else c->arrivals[a->next].prev = a->prev;
    a->next = c->arrival_free; c->arrival_free = index - 1;
    /* Backward-shift hash deletion: no tombstone accumulation on long runs.
     * Nodes remain at stable indices while only bucket indices move. */
    size_t hole = bucket, n = c->arrival_slots;
    for (size_t i = (hole + 1) % n; c->arrival_buckets[i]; i = (i + 1) % n) {
        const struct arrival *v = &c->arrivals[c->arrival_buckets[i] - 1];
        size_t start = arrival_hash(c, v->source, v->position);
        if ((i + n - start) % n >= (i + n - hole) % n) {
            c->arrival_buckets[hole] = c->arrival_buckets[i]; hole = i;
        }
    }
    c->arrival_buckets[hole] = 0;
}
static pgwt_merge_observation observation(struct pgwt_merge_capture *c,
                                         uint64_t d, const ma_snapshot *s)
{
    pgwt_merge_observation o = {.d = d};
    o.counters.known = s && s->valid;
    if (s) for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i) {
        o.counters.value[i] = s->value[i];
        o.counters.known = o.counters.known && s->component_valid[i];
    }
    if (!c->config.writer_failed) {
        uint64_t v = o.counters.value[14];
        if (UINT64_MAX - v < c->local_writer_fail) o.counters.known = false;
        else o.counters.value[14] += c->local_writer_fail;
    }
    return o;
}
static void refresh(struct pgwt_merge_capture *c, pgwt_merge_observation *o)
{
    if (c->config.observe && c->config.observe(c->config.ctx, o) < 0)
        o->counters.known = false;
    if (!c->config.writer_failed) {
        uint64_t v = o->counters.value[14];
        if (UINT64_MAX - v < c->local_writer_fail) o->counters.known = false;
        else o->counters.value[14] += c->local_writer_fail;
    }
}
static bool capture_sink(void *ctx, const pgwt_merge_output *out,
                          pgwt_merge_observation *post)
{
    struct pgwt_merge_capture *c = ctx;
    uint64_t before = post->counters.value[14];
    bool ok = true;
    if (out->kind == PGWT_MERGE_OUT_DATA && out->record.rank == PGWT_MERGE_TRANS &&
        c->config.trace && c->config.trace(c->config.ctx, out->record.payload,
                                          out->record.payload_size) < 0) ok = false;
    if (ok && c->config.sink.emit)
        ok = c->config.sink.emit(c->config.sink.ctx, out, post);
    if (ok && c->config.sink.flush &&
        (out->kind == PGWT_MERGE_OUT_FRONTIER || out->kind == PGWT_MERGE_OUT_VOID))
        ok = c->config.sink.flush(c->config.sink.ctx);
    if (c->config.observe) refresh(c, post);
    if (!ok) {
        /* The existing trace handler already counts its writer rejection.
         * Count a failing additional consumer only if it has not done so. */
        if (c->config.writer_failed) {
            if (!post->counters.known || post->counters.value[14] == before) {
                c->config.writer_failed(c->config.ctx);
                if (c->config.observe) refresh(c, post);
            }
        } else if (post->counters.value[14] == before) {
            if (c->local_writer_fail == UINT64_MAX || post->counters.value[14] == UINT64_MAX)
                post->counters.known = false;
            else { ++c->local_writer_fail; ++post->counters.value[14]; }
        }
    }
    c->observation = *post;
    if (ok && (out->kind == PGWT_MERGE_OUT_DATA || out->kind == PGWT_MERGE_OUT_TEXT))
        arrival_remove(c, &out->record, post->d);
    if (ok && out->kind == PGWT_MERGE_OUT_DATA) {
        ++c->released_data;
        /* Semantic event time to publication: includes kernel-buffer time. */
        if (post->d >= out->record.t) age_add(c->event_hist, &c->event_max, post->d - out->record.t);
    }
    if (ok && out->kind == PGWT_MERGE_OUT_TEXT) ++c->released_text;
    if (ok && out->kind == PGWT_MERGE_OUT_FRONTIER) ++c->frontiers;
    if (out->kind == PGWT_MERGE_OUT_VOID) ++c->voids;
    if (out->kind == PGWT_MERGE_OUT_VOID) {
        held_age(c, out->d);
        arrival_reset(c);
    }
    if (ok && (out->kind == PGWT_MERGE_OUT_DATA || out->kind == PGWT_MERGE_OUT_TEXT) && c->staged)
        --c->staged;
    if (out->kind == PGWT_MERGE_OUT_VOID) c->staged = 0;
    return ok;
}

/* Normally the FRONTIER callback commits the batch inside the core's
 * publication checkpoint. TEXT may publish without advancing F; finish that
 * batch here. A failed append leaves only an uncertified suffix and must
 * still reach the same writer-failure/VOID path. */
static int flush_outputs(struct pgwt_merge_capture *c)
{
    if (!c->config.sink.flush || c->config.sink.flush(c->config.sink.ctx)) return 0;
    pgwt_merge_observation o = c->observation;
    if (c->config.writer_failed) {
        c->config.writer_failed(c->config.ctx);
        refresh(c, &o);
    } else if (c->local_writer_fail == UINT64_MAX || o.counters.value[14] == UINT64_MAX) {
        o.counters.known = false;
    } else { ++c->local_writer_fail; ++o.counters.value[14]; }
    c->observation = o;
    pgwt_merge_input in = {.kind = PGWT_MERGE_WRITER_REJECTED, .observation = o};
    pgwt_merge_status status = pgwt_merge_feed(c->core, &in);
    return status == PGWT_MERGE_EXIT_REQUIRED || pgwt_merge_get_report(c->core).void_failed ? -EIO : 0;
}

static struct pgwt_merge_capture *capture_create(const struct pgwt_capture_config *cfg,
                                                const pgwt_merge_config *initial,
                                                const ma_table *premises)
{
    if (!cfg || !cfg->cpu_count || cfg->cpu_count > 65536 || !cfg->capacity ||
        cfg->capacity > PGWT_CAPTURE_LIFETIME_CAPACITY ||
        cfg->capacity > SIZE_MAX / sizeof(struct retained) ||
        cfg->capacity > SIZE_MAX / 2 / sizeof(struct arrival)) return NULL;
    struct pgwt_merge_capture *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->dump_fd = -1;
    c->oldest = c->newest = SIZE_MAX;
    c->arrival_slots = cfg->capacity * 2;
    c->arrivals = calloc(cfg->capacity, sizeof(*c->arrivals));
    c->arrival_buckets = calloc(c->arrival_slots, sizeof(*c->arrival_buckets));
    c->config = *cfg; c->source_count = cfg->cpu_count + 2;
    c->observation = cfg->baseline;
    c->last_checkpoint_at = cfg->baseline.d;
    c->lifetimes = calloc(cfg->capacity, sizeof(*c->lifetimes));
    c->bindings = calloc(cfg->capacity, sizeof(*c->bindings));
    c->lifetime_index = calloc(c->arrival_slots, sizeof(*c->lifetime_index));
    c->binding_index = calloc(c->arrival_slots, sizeof(*c->binding_index));
    c->held = calloc(c->source_count, sizeof(*c->held));
    c->heads = calloc(c->source_count, sizeof(*c->heads));
    c->tails = calloc(c->source_count, sizeof(*c->tails));
    c->storage = calloc(cfg->capacity, sizeof(*c->storage));
    /* Optional measurement memory: a failed allocation only loses quantiles. */
    c->held_hist = calloc(PGWT_CAPTURE_AGE_BUCKETS + 1, sizeof(*c->held_hist));
    c->event_hist = calloc(PGWT_CAPTURE_AGE_BUCKETS + 1, sizeof(*c->event_hist));
    ma_source_decl *sources = calloc(c->source_count, sizeof(*sources));
    if (!c->lifetime_index || !c->binding_index || !c->arrival_buckets || !c->arrivals || !c->lifetimes || !c->bindings || !c->held || !c->heads || !c->tails || !c->storage || !sources)
        goto fail;
    arrival_reset(c);
    for (size_t i = 0; i < cfg->capacity; ++i) {
        c->storage[i].next = c->free; c->free = &c->storage[i];
    }
    for (size_t i = 0; i < c->source_count; ++i)
        sources[i] = (ma_source_decl){.mechanism = i < cfg->cpu_count ? MA_WATCHPOINT_PERF : MA_ORDERED_RING,
            .qualification_complete = true, .initial_cutoff = initial ? initial->sources[i].cutoff : 0};
    ma_config mc = {.mode = PGWT_MERGE_N, .synthetic_premises = false,
                   .sources = sources, .source_count = c->source_count};
    c->adapter = ma_create_production(&mc, NULL);
    free(sources); sources = NULL;
    if (!c->adapter) goto fail;
    pgwt_merge_config base = {.staged_capacity = cfg->capacity, .payload_capacity = 272,
        .attachment_capacity = cfg->capacity, .frontier_capacity = cfg->capacity,
        .dependency_capacity = cfg->capacity, .sink = capture_sink, .sink_context = c,
        .observation = cfg->baseline, .last_observed = cfg->baseline.counters}, core_config;
    if (initial) {
        base = *initial; base.sources = NULL;
        base.sink = capture_sink; base.sink_context = c;
    }
    if (premises) {
        if (premises->lifetime_count > cfg->capacity || premises->binding_count > cfg->capacity) goto fail;
        memcpy(c->lifetimes, premises->lifetimes, premises->lifetime_count * sizeof(*c->lifetimes));
        memcpy(c->bindings, premises->bindings, premises->binding_count * sizeof(*c->bindings));
        ma_table t = {.lifetimes = c->lifetimes, .bindings = c->bindings,
            .lifetime_index = c->lifetime_index, .binding_index = c->binding_index,
            .index_slots = c->arrival_slots};
        for (size_t i = 0; i < premises->lifetime_count; ++i) {
            size_t slot = ma_lifetime_bucket(&t, c->lifetimes[i].pid, c->lifetimes[i].g, NULL);
            if (c->lifetime_index[slot]) goto fail;
            c->lifetime_index[slot] = i + 1;
        }
        for (size_t i = 0; i < premises->binding_count; ++i) {
            size_t slot = ma_binding_bucket(&t, c->bindings[i].query_id, NULL);
            if (c->binding_index[slot]) goto fail;
            c->binding_index[slot] = i + 1;
        }
        c->lifetime_count = premises->lifetime_count; c->binding_count = premises->binding_count;
    }
    if (!ma_core_config(c->adapter, &base, &core_config)) goto fail;
    pgwt_merge_status status;
    c->core = pgwt_merge_create(&core_config, &status);
    if (!c->core || pgwt_merge_get_report(c->core).void_failed) goto fail;
    return c;
fail:
    free(sources); pgwt_capture_destroy(c); return NULL;
}
struct pgwt_merge_capture *pgwt_capture_create(const struct pgwt_capture_config *cfg)
{
    return capture_create(cfg, NULL, NULL);
}
static ma_table table(struct pgwt_merge_capture *c)
{
    return (ma_table){.lifetimes = c->lifetimes, .lifetime_count = c->lifetime_count,
        .bindings = c->bindings, .binding_count = c->binding_count,
        .held = c->held, .source_count = c->source_count,
        .lifetime_index = c->lifetime_index, .binding_index = c->binding_index,
        .index_slots = c->arrival_slots};
}
#ifdef PGWT_TRANSPORT_HOST_TEST
struct pgwt_merge_capture *pgwt_capture_replay_create(const struct pgwt_capture_config *cfg,
                                                      const pgwt_merge_config *initial,
                                                      const ma_table *premises)
{
    if (!initial || !initial->sources || initial->source_count != cfg->cpu_count + 2) return NULL;
    return capture_create(cfg, initial, premises);
}
pgwt_merge_status pgwt_capture_replay_control(struct pgwt_merge_capture *c, const pgwt_merge_input *in)
{
    return pgwt_merge_feed(c->core, in);
}
ma_table pgwt_capture_replay_table(struct pgwt_merge_capture *c) { return table(c); }
#endif
int pgwt_capture_fault(struct pgwt_merge_capture *c, pgwt_merge_reason reason,
                        uint64_t d, const ma_snapshot *s)
{
    if (!c || c->finished) return -EINVAL;
    c->observation = observation(c, d, s);
    pgwt_merge_input in = {.kind = PGWT_MERGE_FAULT, .reason = reason, .observation = c->observation};
    pgwt_merge_status status = pgwt_merge_feed(c->core, &in);
    return status == PGWT_MERGE_EXIT_REQUIRED || pgwt_merge_get_report(c->core).void_failed ? -EIO : 0;
}
int pgwt_capture_received(struct pgwt_merge_capture *c, const ma_envelope *e, uint64_t now)
{
    if (!c || !e || c->finished) return -EINVAL;
    if (e->kind != MA_LIFECYCLE && e->kind != MA_TEXT && e->kind != MA_TRACE) return 0;
    size_t expected = e->kind == MA_TRACE ? 48 : e->kind == MA_TEXT ? 272 : 24;
    if (!e->bytes || e->size != expected) return -EINVAL;
    /* A TRACE or lifecycle record with no lifetime cookie comes from a
     * backend that existed before this capture (issue 871 comment
     * 6873823454: such backends "stay uncovered and reported"). It has no
     * generation, so it is not merge input: it is not staged, not held and
     * never published, and the caller counts it. Its legacy side effect
     * (an EXIT retiring the daemon's backend entry) still runs below.
     * A FRONTIER therefore says nothing about uncovered backends. */
    bool uncovered = e->kind != MA_TEXT && !(e->cookie_present && e->cookie);
    if (!uncovered && pgwt_capture_report(c).status == PGWT_MERGE_OK) {
        size_t slot = arrival_slot(c, e->source, e->position);
        if (c->arrival_free == SIZE_MAX) return -ENOMEM;
        if (c->arrival_buckets[slot]) {
            ma_snapshot snap = pgwt_capture_last_snapshot(c);
            return pgwt_capture_fault(c, PGWT_MERGE_DUPLICATE_IDENTITY, now, &snap);
        }
        size_t index = c->arrival_free;
        c->arrival_free = c->arrivals[index].next;
        c->arrivals[index] = (struct arrival){.source = e->source, .position = e->position,
            .at = now, .prev = c->newest, .next = SIZE_MAX};
        c->arrival_buckets[slot] = index + 1;
        if (c->newest == SIZE_MAX) c->oldest = index;
        else c->arrivals[c->newest].next = index;
        c->newest = index;
    }
    if (e->kind == MA_LIFECYCLE && get_le(e->bytes, 4) == 1 && e->cookie_present && e->cookie) {
        uint64_t pid = get_le(e->bytes + 4, 4);
        ma_table t = table(c);
        size_t slot = ma_lifetime_bucket(&t, pid, e->cookie, NULL);
        if (!c->lifetime_index[slot]) {
            if (c->lifetime_count == c->config.capacity) return -ENOMEM;
            c->lifetimes[c->lifetime_count++] = (ma_lifetime){.pid = pid,
                .g = e->cookie, .identity_bound = true, .proof = MA_NOT_ATTEMPTED};
            c->lifetime_index[slot] = c->lifetime_count;
            if (c->config.test_attach_premise) {
                /* TEST PREMISE, never a production fact: the attachment
                 * proof (unit A2) does not exist yet, so nothing reports
                 * BEGIN/COMPLETE and every fork-seeded generation would VOID
                 * at its first TRANS (contract row 33). The transport-only
                 * box test ASSUMES the twelve facts here, through the same
                 * report path A2 will use, at FORK consumption. */
                ma_snapshot snap = pgwt_capture_last_snapshot(c);
                struct pgwt_attach_report r = {.pid = (uint32_t)pid, .cookie = e->cookie, .event = MA_BEGIN};
                int rc = pgwt_capture_attach(c, &r, now, &snap);
                if (rc < 0) return rc;
                r.event = MA_COMPLETE; r.facts_known = r.facts_true = MA_ALL_FACTS;
                snap = pgwt_capture_last_snapshot(c);
                if ((rc = pgwt_capture_attach(c, &r, now, &snap)) < 0) return rc;
            }
        }
    }
    uint64_t query = uncovered ? 0 : e->kind == MA_TRACE ? get_le(e->bytes + 32, 8) :
                     e->kind == MA_TEXT ? get_le(e->bytes + 8, 8) : 0;
    if (query) {
        ma_table t = table(c);
        size_t slot = ma_binding_bucket(&t, query, NULL);
        if (!c->binding_index[slot]) {
            if (c->binding_count == c->config.capacity) return -ENOMEM;
            c->bindings[c->binding_count++] = (ma_binding){.query_id = query, .kind = MA_NOT_REQUIRED};
            c->binding_index[slot] = c->binding_count;
        }
    }
    if ((e->kind == MA_LIFECYCLE || e->kind == MA_TEXT) && c->config.lifecycle) {
        int rc = c->config.lifecycle(c->config.ctx, e->bytes, e->size);
        if (rc < 0) {
            if (c->config.writer_failed) c->config.writer_failed(c->config.ctx);
            else if (c->local_writer_fail != UINT64_MAX) ++c->local_writer_fail;
            return rc;
        }
    }
    return uncovered ? 1 : 0;
}
ma_snapshot pgwt_capture_last_snapshot(const struct pgwt_merge_capture *c)
{
    ma_snapshot s = {0};
    if (!c) return s;
    s.valid = c->observation.counters.known;
    for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i) {
        s.value[i] = c->observation.counters.value[i]; s.component_valid[i] = s.valid;
    }
    /* Portable callback accounting is added exactly once by observation(). */
    if (!c->config.writer_failed) {
        if (s.value[14] < c->local_writer_fail) s.valid = false;
        else s.value[14] -= c->local_writer_fail;
    }
    return s;
}
static int mapped(struct pgwt_merge_capture *c, const ma_envelope *e,
                    uint64_t d, ma_snapshot *s, bool *hold)
{
    ma_table t = table(c);
    ma_snapshot snap = {0};
    c->observation = observation(c, d, s);
    snap.valid = c->observation.counters.known;
    for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i) {
        snap.value[i] = c->observation.counters.value[i]; snap.component_valid[i] = snap.valid;
    }
    ma_result r = ma_map(c->adapter, e, &t, d, &snap);
    *hold = r.hold;
    for (size_t i = 0; i < r.count; ++i) {
        r.input[i].observation = c->observation;
        if (r.input[i].kind == PGWT_MERGE_DATA || r.input[i].kind == PGWT_MERGE_TEXT) ++c->staged;
        if (pgwt_merge_feed(c->core, &r.input[i]) == PGWT_MERGE_EXIT_REQUIRED ||
            pgwt_merge_get_report(c->core).void_failed) return -EIO;
        if (flush_outputs(c) < 0) return -EIO;
    }
    if (r.has_lifetime_update) {
        size_t slot = ma_lifetime_bucket(&t, r.next_lifetime.pid, r.next_lifetime.g, NULL);
        if (c->lifetime_index[slot])
            c->lifetimes[c->lifetime_index[slot] - 1] = r.next_lifetime;
    }
    *s = pgwt_capture_last_snapshot(c);
    return 0;
}
static int flush_source(struct pgwt_merge_capture *c, size_t source,
                         uint64_t d, ma_snapshot *s)
{
    while (c->heads[source]) {
        struct retained *n = c->heads[source];
        bool hold;
        int rc = mapped(c, &n->envelope, d, s, &hold);
        if (rc < 0) return rc;
        if (hold && pgwt_capture_report(c).status == PGWT_MERGE_OK) {
            c->held[source] = &n->envelope; return 0;
        }
        c->held[source] = NULL;
        c->heads[source] = n->next;
        if (!n->next) c->tails[source] = NULL;
        n->next = c->free; c->free = n; --c->retained_count;
    }
    return 0;
}
int pgwt_capture_envelope(struct pgwt_merge_capture *c, const ma_envelope *e,
                           uint64_t d, const ma_snapshot *s)
{
    if (!c || !e || c->finished) return -EINVAL;
    ma_snapshot current = s ? *s : (ma_snapshot){0};
    if (e->kind != MA_TRACE && e->kind != MA_LIFECYCLE && e->kind != MA_TEXT && e->kind != MA_BOUNDARY) {
        bool hold;
        int rc = mapped(c, e, d, &current, &hold);
        if (rc < 0) return rc;
        for (size_t i = 0; i < c->source_count; ++i)
            if ((rc = flush_source(c, i, d, &current)) < 0) return rc;
        return 0;
    }
    if (e->source >= c->source_count || e->size > 272 || (e->size && !e->bytes))
        return pgwt_capture_fault(c, PGWT_MERGE_MALFORMED_ENVELOPE, d, s);
    if (!c->free) return pgwt_capture_fault(c, PGWT_MERGE_ALLOCATION_FAIL, d, s);
    struct retained *n = c->free; c->free = n->next; n->next = NULL;
    n->envelope = *e;
    if (e->size) { memcpy(n->bytes, e->bytes, e->size); n->envelope.bytes = n->bytes; }
    if (c->tails[e->source]) c->tails[e->source]->next = n;
    else c->heads[e->source] = n;
    c->tails[e->source] = n; ++c->retained_count;
    return flush_source(c, e->source, d, &current);
}
int pgwt_capture_attach(struct pgwt_merge_capture *c, const struct pgwt_attach_report *r,
                         uint64_t d, const ma_snapshot *s)
{
    if (!r) return -EINVAL;
    ma_envelope e = {.version = 1, .kind = MA_ATTACH_EVENT, .control_pid = r->pid,
        .cookie = r->cookie, .cookie_present = r->cookie != 0,
        .attach_event = (ma_attach_event)r->event,
        .facts_known = r->facts_known, .facts_true = r->facts_true};
    return pgwt_capture_envelope(c, &e, d, s);
}
uint64_t pgwt_capture_max_held_ns(const struct pgwt_merge_capture *c, uint64_t now)
{
    if (!c) return 0;
    uint64_t max = c->max_held;
    if (c->oldest != SIZE_MAX && now >= c->arrivals[c->oldest].at && now - c->arrivals[c->oldest].at > max)
        max = now - c->arrivals[c->oldest].at;
    return max;
}
bool pgwt_capture_waiting(const struct pgwt_merge_capture *c)
{ return c && (c->retained_count || c->staged) && pgwt_capture_report(c).status == PGWT_MERGE_OK; }
size_t pgwt_capture_pending_records(const struct pgwt_merge_capture *c)
{ return c ? c->retained_count + c->staged : 0; }
pgwt_merge_report pgwt_capture_report(const struct pgwt_merge_capture *c)
{ return pgwt_merge_get_report(c ? c->core : NULL); }
static int finish_capture(struct pgwt_merge_capture *c, uint64_t d, const ma_snapshot *s)
{
    if (!c) return -EINVAL;
    if (c->finished) return 0;
    ma_envelope check = {.version = 1, .kind = MA_CHECK};
    int rc = pgwt_capture_envelope(c, &check, d, s);
    if (rc < 0) return rc;
    if (pgwt_capture_waiting(c)) rc = pgwt_capture_fault(c, PGWT_MERGE_UNKNOWN_EVIDENCE, d, s);
    return rc;
}
int pgwt_capture_finish(struct pgwt_merge_capture *c, uint64_t d, const ma_snapshot *s)
{
    int rc = finish_capture(c, d, s);
    if (c) c->finished = true;
    return rc;
}
void pgwt_capture_destroy(struct pgwt_merge_capture *c)
{
    if (!c) return;
    pgwt_merge_destroy(c->core); ma_destroy(c->adapter);
    free(c->lifetimes); free(c->bindings); free(c->held); free(c->heads); free(c->tails);
    free(c->lifetime_index); free(c->binding_index);
    free(c->held_hist); free(c->event_hist);
    free(c->storage); free(c->arrivals); free(c->arrival_buckets); free(c);
}
struct pgwt_capture_stats pgwt_capture_stats(const struct pgwt_merge_capture *c)
{
    struct pgwt_capture_stats s = {0};
    if (!c) return s;
    s.released_data = c->released_data; s.released_text = c->released_text;
    s.frontiers = c->frontiers; s.voids = c->voids;
    s.held_max_ns = c->held_max; s.event_max_ns = c->event_max;
    s.held_p50_ns = age_quantile(c->held_hist, 5000); s.held_p99_ns = age_quantile(c->held_hist, 9900);
    s.event_p50_ns = age_quantile(c->event_hist, 5000); s.event_p99_ns = age_quantile(c->event_hist, 9900);
    return s;
}
/* One allocation at open; no record-path allocation, formatting or stdio. */
struct pgwt_capture_dump {
    unsigned char *bytes;
    size_t used, capacity;
    pgwt_capture_write_fn write;
    void *ctx;
};
static void put_le(unsigned char *p, uint64_t v, size_t n)
{
    for (size_t i = 0; i < n; ++i) p[i] = (unsigned char)(v >> (8 * i));
}
struct pgwt_capture_dump *pgwt_capture_dump_create(size_t records,
                                                  pgwt_capture_write_fn write, void *ctx)
{
    if (!records || !write || records > (SIZE_MAX - 152) / 312) return NULL;
    struct pgwt_capture_dump *dump = calloc(1, sizeof(*dump));
    if (!dump) return NULL;
    dump->capacity = records * 312 + 152;
    dump->bytes = malloc(dump->capacity);
    if (!dump->bytes) { free(dump); return NULL; }
    dump->write = write; dump->ctx = ctx;
    return dump;
}
bool pgwt_capture_dump_flush(void *ctx)
{
    struct pgwt_capture_dump *dump = ctx;
    if (!dump) return false;
    if (!dump->used) return true;
    bool ok = dump->write(dump->ctx, dump->bytes, dump->used);
    /* Failed suffixes must not be retried as valid DATA along with the VOID.
     * A partial physical append is explicitly an uncertified tail. */
    dump->used = 0;
    return ok;
}
bool pgwt_capture_dump_header(struct pgwt_capture_dump *dump, const uint32_t *cpus, size_t n)
{
    if (!dump || !cpus || !n || n > 65536 || dump->used ||
        n > (dump->capacity - 24) / 4) return false;
    unsigned char *p = dump->bytes;
    memcpy(p, "PGWTQB02", 8);
    put_le(p + 8, 2, 4); put_le(p + 12, n, 4);
    put_le(p + 16, PGWT_CAPTURE_LIFETIME_CAPACITY, 4);
    put_le(p + 20, PGWT_CAPTURE_DUMP_TRACE_BYTES, 4);
    for (size_t i = 0; i < n; ++i) put_le(p + 24 + i * 4, cpus[i], 4);
    dump->used = 24 + n * 4;
    return pgwt_capture_dump_flush(dump);
}
bool pgwt_capture_dump(void *ctx, const pgwt_merge_output *out, pgwt_merge_observation *post)
{
    (void)post;
    struct pgwt_capture_dump *dump = ctx;
    if (!dump || !out || (unsigned)out->kind > PGWT_MERGE_OUT_VOID) return false;
    bool data = out->kind == PGWT_MERGE_OUT_DATA || out->kind == PGWT_MERGE_OUT_TEXT;
    const pgwt_merge_record *r = &out->record;
    if (data && (r->payload_size > 272 || (r->payload_size && !r->payload) ||
        r->source > UINT32_MAX || r->q || r->has_q)) return false;
    size_t n = data ? 40 + r->payload_size : 152;
    if (n > dump->capacity - dump->used) return false;
    unsigned char *p = dump->bytes + dump->used;
    memset(p, 0, data ? 40 : n);
    put_le(p, n, 2); p[2] = (unsigned char)out->kind;
    put_le(p + 8, out->epoch, 8);
    if (data) {
        p[3] = (unsigned char)(out->prior_interval_certified | ((unsigned)r->rank << 1) |
            (r->identity_known << 3) | (r->fork_seed_proved << 4) | ((unsigned)r->dependency << 5));
        put_le(p + 4, r->source, 4); put_le(p + 16, r->o, 8);
        put_le(p + 24, r->g, 8); put_le(p + 32, r->dependency_id, 8);
        if (r->payload_size) memcpy(p + 40, r->payload, r->payload_size);
    } else {
        p[3] = out->counters.known;
        if (out->kind == PGWT_MERGE_OUT_VOID) put_le(p + 4, out->reason, 4);
        put_le(p + 16, out->F, 8); put_le(p + 24, out->d, 8);
        for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i)
            put_le(p + 32 + i * 8, out->counters.value[i], 8);
    }
    dump->used += n;
    return data || pgwt_capture_dump_flush(dump);
}
bool pgwt_capture_dump_end(struct pgwt_capture_dump *dump, uint64_t age,
                           bool known, uint64_t lost, pgwt_merge_report report)
{
    if (!dump || dump->capacity - dump->used < 32) return false;
    unsigned char *p = dump->bytes + dump->used;
    memset(p, 0, 32); put_le(p, 32, 2); p[2] = 4;
    p[3] = (unsigned char)(known | (report.epoch_invalid << 1));
    put_le(p + 8, age, 8); put_le(p + 16, lost, 8); put_le(p + 24, report.status, 4);
    dump->used += 32;
    return pgwt_capture_dump_flush(dump);
}
void pgwt_capture_dump_destroy(struct pgwt_capture_dump *dump)
{
    if (dump) { free(dump->bytes); free(dump); }
}

/* POSIX daemon binding: compile this on the Mac as well. Kernel transport
 * access remains behind percpu_transport's injectable interface. */
#include "daemon.h"
#include "capture_counters.h"
#include "event_stream.h"
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

int pgwt_merge_capture_snapshot(struct pgwt_daemon *d, ma_snapshot *s, uint64_t *now)
{
    if (!d || !s || !now) return -EINVAL;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -errno;
    *now = (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
    struct pgwt_counter_vector v = pgwt_daemon_counter_vector(d);
    _Static_assert(PGWT_COUNTER_COMPONENTS == PGWT_MERGE_COUNTERS, "vector size");
    memset(s, 0, sizeof(*s)); s->valid = v.valid;
    for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i) {
        s->value[i] = v.components[i].value; s->component_valid[i] = v.components[i].valid;
    }
    if (d->merge_capture) d->merge_capture->last_checkpoint_at = *now;
    return 0;
}
static int daemon_observe(void *ctx, pgwt_merge_observation *o)
{
    struct pgwt_daemon *d = ctx;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -errno;
    o->d = (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
    /* The other 14 components are the completed-round snapshot. Only this
     * component can change synchronously in a publication callback. */
    o->counters.value[PGWT_C_WRITER_PUSH_FAIL] = d->counters.writer_push_fail_total;
    return 0;
}
static bool daemon_write(void *ctx, const void *bytes, size_t n)
{
    return write((int)(intptr_t)ctx, bytes, n) == (ssize_t)n;
}
static bool daemon_flush(void *ctx)
{
    struct pgwt_daemon *d = ctx;
    if (d->merge_sink.flush && !d->merge_sink.flush(d->merge_sink.ctx)) return false;
    return d->merge_capture && pgwt_capture_dump_flush(d->merge_capture->dump);
}
static int daemon_lifecycle(void *ctx, const void *p, size_t n)
{
    _Alignas(8) unsigned char copy[272];
    if (n > sizeof(copy)) return -EINVAL;
    memcpy(copy, p, n);
    return pgwt_process_lifecycle_payload(ctx, copy, n);
}
static int daemon_trace(void *ctx, const void *p, size_t n)
{
    struct pgwt_daemon *d = ctx;
    struct pgwt_trace_event copy;
    if (n != sizeof(copy)) return -EINVAL;
    memcpy(&copy, p, sizeof(copy));
    uint64_t before = d->counters.writer_push_fail_total;
    int rc = pgwt_process_trace_payload(d, &copy, sizeof(copy));
    return rc < 0 || d->counters.writer_push_fail_total != before ? -EIO : 0;
}
static void daemon_writer_failed(void *ctx)
{
    struct pgwt_daemon *d = ctx;
    ++d->counters.writer_push_fail_total;
}
static bool daemon_sink(void *ctx, const pgwt_merge_output *out, pgwt_merge_observation *post)
{
    struct pgwt_daemon *d = ctx;
    /* Another consumer (the later capture writer) can register here. */
    bool ok = !d->merge_sink.emit || d->merge_sink.emit(d->merge_sink.ctx, out, post);
    /* Required consumers commit before the dump can expose a FRONTIER. */
    if (ok && d->merge_sink.flush &&
        (out->kind == PGWT_MERGE_OUT_FRONTIER || out->kind == PGWT_MERGE_OUT_VOID))
        ok = d->merge_sink.flush(d->merge_sink.ctx);
    if (ok) ok = d->merge_capture && pgwt_capture_dump(d->merge_capture->dump, out, post);
    if (out->kind == PGWT_MERGE_OUT_VOID) {
        fprintf(stderr, "exact-perf: suspended: %s at %llu; automatic REARM disabled\n",
            pgwt_merge_reason_name(out->reason), (unsigned long long)out->d);
        if (!ok) fprintf(stderr, "exact-perf: VOID could not be persisted; capture stopped\n");
    }
    return ok;
}
static int daemon_attach(void *ctx, const struct pgwt_attach_report *r)
{ return pgwt_merge_capture_attach(ctx, r); }
int pgwt_merge_capture_open(struct pgwt_daemon *d)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    if (d->merge_capture) return 0;
    if (!d->trace_dir) {
        fprintf(stderr, "exact-perf: qualification sink requires a trace directory\n");
        return -EINVAL;
    }
    /* CPU count is supplied by the already prepared transport. */
    size_t ncpu = pgwt_percpu_transport_cpu_count(d);
    if (!ncpu) return -EINVAL;
    pgwt_merge_observation baseline = {0};
    ma_snapshot initial;
    if (pgwt_percpu_transport_snapshot(d, &initial, &baseline.d) < 0) return -EIO;
    baseline.counters.known = initial.valid;
    for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i) {
        baseline.counters.value[i] = initial.value[i];
        baseline.counters.known &= initial.component_valid[i];
    }
    if (!baseline.counters.known) return -EIO;
    const char *premise = getenv("PGWT_TEST_EXACT_ATTACH_PREMISE");
    bool assumed = premise && !strcmp(premise, "1");
    if (assumed)
        fprintf(stderr, "exact-perf: TEST PREMISE (PGWT_TEST_EXACT_ATTACH_PREMISE=1): attachment proofs "
            "are ASSUMED for every fork-seeded backend; unit A2 does not exist; this capture "
            "certifies nothing\n");
    struct pgwt_capture_config cfg = {.cpu_count = ncpu, .capacity = PGWT_CAPTURE_LIFETIME_CAPACITY,
        .test_attach_premise = assumed,
        .baseline = baseline, .ctx = d, .lifecycle = daemon_lifecycle,
        .trace = daemon_trace, .observe = daemon_observe, .writer_failed = daemon_writer_failed,
        .sink = {.emit = daemon_sink, .ctx = d, .flush = daemon_flush}};
    /* Create the dump first: creation can report a VOID through the sink. */
    size_t len = strlen(d->trace_dir);
    if (len > SIZE_MAX - 40) return -ENAMETOOLONG;
    char *path = malloc(len + 40);
    if (!path) return -ENOMEM;
    snprintf(path, len + 40, "%s/merge-qualification.bin", d->trace_dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600); free(path);
    if (fd < 0) return -errno;
    struct pgwt_capture_dump *dump = pgwt_capture_dump_create(cfg.capacity, daemon_write, (void *)(intptr_t)fd);
    uint32_t *cpus = malloc(ncpu * sizeof(*cpus));
    if (!dump || !cpus) {
        free(cpus); pgwt_capture_dump_destroy(dump); close(fd); return -ENOMEM;
    }
    for (size_t i = 0; i < ncpu; ++i) cpus[i] = pgwt_percpu_transport_cpu_id(d, i);
    bool header_ok = pgwt_capture_dump_header(dump, cpus, ncpu);
    free(cpus);
    if (!header_ok) { pgwt_capture_dump_destroy(dump); close(fd); return -EIO; }
    /* Validated baseline and fixed capacities make ordinary construction
     * silent; a creation failure is reported out of band, never half-wired. */
    d->merge_capture = pgwt_capture_create(&cfg);
    if (!d->merge_capture) {
        fprintf(stderr, "exact-perf: production merge creation failed; capture stopped\n");
        pgwt_capture_dump_destroy(dump); close(fd); return -ENOMEM;
    }
    d->merge_capture->dump = dump; d->merge_capture->dump_fd = fd;
    fprintf(stderr, "exact-perf: retaining up to %u lifetimes until capture teardown; "
        "exact capture pauses with VOID(allocation_fail) at capacity; no retirement\n",
        PGWT_CAPTURE_LIFETIME_CAPACITY);
    d->attach_report = daemon_attach; d->attach_report_ctx = d;
    return 0;
}
int pgwt_merge_capture_poll(struct pgwt_daemon *d)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    if (!d->merge_capture) return -EINVAL;
    ma_snapshot s = pgwt_capture_last_snapshot(d->merge_capture);
    pgwt_merge_observation o = d->merge_capture->observation;
    int rc = daemon_observe(d, &o);
    uint64_t now = o.d;
    if (rc >= 0 && !pgwt_transport_service_due(now, d->merge_capture->last_service_at,
                                             d->merge_capture->serviced)) return 0;
    d->merge_capture->last_service_at = now; d->merge_capture->serviced = true;
    s.value[PGWT_C_WRITER_PUSH_FAIL] = o.counters.value[PGWT_C_WRITER_PUSH_FAIL];
    if (rc < 0) s.valid = false;
    /* A full vector is read at open, after the completion fence, on faults,
     * at shutdown, and every 10 ms of transport silence. Never reread maps
     * just because the main loop or a publication callback ran. A missing
     * helper cannot block silence checks indefinitely. */
    const uint64_t silence_ns = UINT64_C(10000000);
    if (now < d->merge_capture->last_checkpoint_at ||
        (now - d->merge_capture->last_checkpoint_at >= silence_ns &&
         pgwt_transport_silent(d->percpu_transport, now, silence_ns))) {
        if (pgwt_percpu_transport_snapshot(d, &s, &now) < 0) s.valid = false;
    }
    ma_envelope e = {.version = 1, .kind = MA_CHECK};
    return pgwt_capture_envelope(d->merge_capture, &e, now, &s);
}
int pgwt_merge_capture_tick(struct pgwt_daemon *d)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    pgwt_tick_duties(d);
    if (!d->running) return 0;
    return pgwt_merge_capture_poll(d);
}
uint64_t pgwt_merge_capture_pending_records(const struct pgwt_daemon *d)
{
    if (!d || !d->exact_perf) return 0;
    return (uint64_t)pgwt_transport_pending_records(d->percpu_transport) +
           pgwt_capture_pending_records(d->merge_capture);
}
int pgwt_merge_capture_finish(struct pgwt_daemon *d)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    if (!d->merge_capture || !d->percpu_transport) return -EINVAL;
    int rc = pgwt_transport_finish(d->percpu_transport);
    ma_snapshot s = {0}; uint64_t now = 0;
    pgwt_percpu_transport_snapshot(d, &s, &now);
    int end = finish_capture(d->merge_capture, now, &s);
    uint64_t age = pgwt_capture_max_held_ns(d->merge_capture, now), lost = 0;
    bool lost_known = pgwt_transport_lost_total(d->percpu_transport, &lost);
    pgwt_merge_report report = pgwt_capture_report(d->merge_capture);
    if (!pgwt_capture_dump_end(d->merge_capture->dump, age, lost_known, lost, report)) {
        ++d->counters.writer_push_fail_total;
        s.value[PGWT_C_WRITER_PUSH_FAIL] = d->counters.writer_push_fail_total;
        end = pgwt_capture_fault(d->merge_capture, PGWT_MERGE_WRITER_PUSH_FAIL, now, &s);
        fprintf(stderr, "exact-perf: qualification summary could not be written\n");
        if (!end) end = -EIO;
    }
    d->merge_capture->finished = true;
    report = pgwt_capture_report(d->merge_capture);
    fprintf(stderr, "exact-perf: max held age ns=%llu; status=%u; perf LOST=%llu%s\n",
        (unsigned long long)age, (unsigned)report.status, (unsigned long long)lost,
        lost_known ? "" : " (unknown overflow)");
    /* Measurements for the box runs, one parseable line each. Not gates. */
    struct pgwt_transport_stats ts = pgwt_transport_stats(d->percpu_transport);
    struct pgwt_capture_stats cs = pgwt_capture_stats(d->merge_capture);
#define U(v) ((unsigned long long)(v))
    fprintf(stderr, "exact-perf: stats rounds_requested=%llu rounds_completed=%llu "
        "rounds_completed_nonempty=%llu holds=%llu cuts=%llu terminal_discards=%llu skipped=%llu "
        "uncovered=%llu wire_records=%llu queue_high=%llu round_us_mean=%llu round_us_max=%llu "
        "cpus=%llu acks_at_completion=%llu acks_before_fence=%llu "
        "released_data=%llu released_text=%llu frontiers=%llu voids=%llu "
        "held_us_p50=%llu held_us_p99=%llu held_us_max=%llu "
        "event_us_p50=%llu event_us_p99=%llu event_us_max=%llu "
        "seal_violation=%llu sequence_gap=%llu unknown_evidence=%llu perf_lost=%llu status=%u\n",
        U(ts.rounds_requested), U(ts.rounds_completed), U(ts.rounds_completed_nonempty),
        U(ts.holds), U(ts.cuts), U(ts.terminal_discards), U(ts.skipped), U(ts.uncovered),
        U(ts.records), U(ts.queue_high),
        U(ts.rounds_completed ? ts.round_ns_total / ts.rounds_completed / 1000 : 0),
        U(ts.round_ns_max / 1000), U(pgwt_percpu_transport_cpu_count(d)),
        U(ts.acks_at_completion), U(ts.acks_before_fence), U(cs.released_data), U(cs.released_text),
        U(cs.frontiers), U(cs.voids), U(cs.held_p50_ns / 1000), U(cs.held_p99_ns / 1000),
        U(cs.held_max_ns / 1000), U(cs.event_p50_ns / 1000), U(cs.event_p99_ns / 1000),
        U(cs.event_max_ns / 1000), U(report.faults[PGWT_MERGE_SEAL_VIOLATION]),
        U(report.faults[PGWT_MERGE_SEQUENCE_GAP]), U(report.faults[PGWT_MERGE_UNKNOWN_EVIDENCE]),
        U(lost), (unsigned)report.status);
    static const char *const names[3] = {"CPU", "E", "L"};
    for (size_t i = 0; i < 3; ++i)
        fprintf(stderr, "exact-perf: reconcile dest=%s attempted=%llu delivered=%llu failures=%llu "
            "discards=%llu read=%d ok=%d\n", names[i], U(ts.final_attempts[i]), U(ts.final_delivered[i]),
            U(ts.final_failures[i]), U(ts.final_discards[i]), ts.final_read,
            ts.final_read && !ts.final_failures[i] &&
            ts.final_attempts[i] == ts.final_delivered[i] + ts.final_discards[i]);
#undef U
    return rc < 0 ? rc : end;
}
int pgwt_merge_capture_record(struct pgwt_daemon *d, size_t source, const void *p, size_t n)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    size_t ncpu = pgwt_percpu_transport_cpu_count(d);
    if (source >= ncpu + 2) return -EINVAL;
    uint32_t dest = source < ncpu ? PGWT_WIRE_DEST_CPU :
                    source == ncpu ? PGWT_WIRE_DEST_E : PGWT_WIRE_DEST_L;
    return pgwt_percpu_transport_record(d, dest, p, n);
}
int pgwt_merge_capture_attach(struct pgwt_daemon *d, const struct pgwt_attach_report *r)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    if (!d->merge_capture) return -EINVAL;
    /* Attachment reports are per lifetime, not a counter-map checkpoint. */
    pgwt_merge_observation o = d->merge_capture->observation;
    if (daemon_observe(d, &o) < 0) o.counters.known = false;
    ma_snapshot s = pgwt_capture_last_snapshot(d->merge_capture);
    s.value[PGWT_C_WRITER_PUSH_FAIL] = o.counters.value[PGWT_C_WRITER_PUSH_FAIL];
    s.valid &= o.counters.known;
    return pgwt_capture_attach(d->merge_capture, r, o.d, &s);
}
void pgwt_merge_capture_close(struct pgwt_daemon *d)
{
    if (!d || !d->merge_capture) return;
    struct pgwt_capture_dump *dump = d->merge_capture->dump;
    int fd = d->merge_capture->dump_fd;
    d->attach_report = NULL; d->attach_report_ctx = NULL;
    pgwt_capture_destroy(d->merge_capture); d->merge_capture = NULL;
    pgwt_capture_dump_destroy(dump);
    if (fd >= 0 && close(fd)) fprintf(stderr, "exact-perf: qualification sink close failed\n");
}
