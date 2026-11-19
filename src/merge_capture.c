#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Production merge feed and qualification-only append sink. */
#include "merge_capture.h"
#include "pg_wait_tracer.h"
#include "transport_diagnostics.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct retained {
    ma_envelope envelope;
    unsigned char bytes[272];
    struct retained *next;
    size_t token;
};
struct arrival {
    uint64_t source, position, at;
    unsigned char prefix[sizeof(uint64_t)];
    bool active;
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
    size_t staged, next_source;
    uint64_t local_writer_fail, max_held, last_checkpoint_at;
    struct arrival *arrivals;
    size_t arrival_slots, oldest, newest, arrival_free, arrival_count;
    pgwt_merge_diagnostics diagnostics;
    uint64_t *arrival_positions;
    /* Copies of accepted input promises, used only to route a late pre-proof
     * record back to the unchanged core for validation/invalidation. These
     * are never release cutoffs, and survive discard/REARM with the core. */
    uint64_t *arrival_promises;
#ifdef PGWT_TRANSPORT_HOST_TEST
    size_t arrival_probes;
#endif
    pgwt_merge_observation observation;
    ma_snapshot snapshot;
    bool finished, serviced;
    bool capacity_paused, pause_open, backlog_discarded;
    uint64_t capture_start, coverage_cursor, covered_ns, pause_ns;
    uint64_t pause_start, pause_end, pauses, recovery_attempts, rearms, proof_acquisitions;
    uint64_t rearm_at, payload_records, dropped_records, uncovered_records, dropped_boundaries;
    pgwt_merge_reason pause_reason;
    pgwt_pool pause_pool;
    pgwt_merge_attachment *fresh_proofs;
    uint64_t last_service_at;
    struct pgwt_transport_budget budget;
    /* Measurements only. Ages in PGWT_CAPTURE_AGE_BUCKET_NS buckets; the
     * last bucket collects everything at or beyond the range. */
    uint64_t released_data, released_text, frontiers, voids;
    uint64_t source_budget_yields, sweep_budget_yields, unfinished_records;
    uint64_t held_max, event_max, event_max_pid;
    unsigned event_max_rank, event_max_source;
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
/* Transport positions increase per source. Keep the old exact duplicate
 * check for callers that repeat/regress positions, including host fixtures.
 * The ordinary path does not probe an index at either end of a record. */
static size_t arrival_find(struct pgwt_merge_capture *c, size_t source, uint64_t position)
{
#ifdef PGWT_TRANSPORT_HOST_TEST
    ++c->arrival_probes;
#endif
    for (size_t i = c->oldest; i != SIZE_MAX; i = c->arrivals[i].next) {
        const struct arrival *a = &c->arrivals[i];
        if (a->source == source && a->position == position) return i + 1;
    }
    return 0;
}
static void held_age(struct pgwt_merge_capture *c, uint64_t now)
{
    if (c->oldest != SIZE_MAX && now >= c->arrivals[c->oldest].at &&
        now - c->arrivals[c->oldest].at > c->max_held)
        c->max_held = now - c->arrivals[c->oldest].at;
}
static void arrival_reset(struct pgwt_merge_capture *c)
{
    memset(c->arrival_positions, 0, c->source_count * sizeof(*c->arrival_positions));
    for (size_t i = 0; i < c->config.capacity; ++i) c->arrivals[i].active = false;
    for (size_t i = 0; i < c->config.capacity; ++i)
        c->arrivals[i].next = i + 1 == c->config.capacity ? SIZE_MAX : i + 1;
    c->arrival_count = 0;
    c->arrival_free = 0; c->oldest = c->newest = SIZE_MAX;
}
static void arrival_remove(struct pgwt_merge_capture *c, size_t index, uint64_t now)
{
    if (!index) return;
    --c->arrival_count;
    held_age(c, now);
    struct arrival *a = &c->arrivals[index - 1];
    /* This record's own age from wire consumption to publication. */
    if (now >= a->at) age_add(c->held_hist, &c->held_max, now - a->at);
    if (a->prev == SIZE_MAX) c->oldest = a->next;
    else c->arrivals[a->prev].next = a->next;
    if (a->next == SIZE_MAX) c->newest = a->prev;
    else c->arrivals[a->next].prev = a->prev;
    a->next = c->arrival_free; c->arrival_free = index - 1;
    a->active = false;
}
static void observation(struct pgwt_merge_capture *c, uint64_t d, const ma_snapshot *s)
{
    pgwt_merge_observation *o = &c->observation;
    o->d = d;
    memset(&o->counters, 0, sizeof(o->counters));
    o->counters.known = s && s->valid;
    if (s) for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i) {
        o->counters.value[i] = s->value[i];
        o->counters.known = o->counters.known && s->component_valid[i];
    }
    if (!c->config.writer_failed) {
        uint64_t v = o->counters.value[14];
        if (UINT64_MAX - v < c->local_writer_fail) o->counters.known = false;
        else o->counters.value[14] += c->local_writer_fail;
    }
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
static void retain_snapshot(struct pgwt_merge_capture *);
static bool capture_sink(void *ctx, const pgwt_merge_output *out,
                          pgwt_merge_observation *post)
{
    struct pgwt_merge_capture *c = ctx;
    size_t token = 0;
    pgwt_merge_output restored;
    unsigned char payload[272];
    if ((out->kind == PGWT_MERGE_OUT_DATA || out->kind == PGWT_MERGE_OUT_TEXT) &&
        out->record.payload_size >= sizeof(uint64_t)) {
        uint64_t id;
        memcpy(&id, out->record.payload, sizeof(id));
        if (id && id <= c->config.capacity) {
            const struct arrival *a = &c->arrivals[id - 1];
            if (a->active && a->source == out->record.source && a->position == out->record.o) {
                token = (size_t)id;
                restored = *out;
                memcpy(payload, out->record.payload, out->record.payload_size);
                memcpy(payload, a->prefix, sizeof(a->prefix));
                restored.record.payload = payload;
                out = &restored;
            }
        }
    }
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
    retain_snapshot(c);
    if (ok && (out->kind == PGWT_MERGE_OUT_DATA || out->kind == PGWT_MERGE_OUT_TEXT))
        arrival_remove(c, token, post->d);
    if (ok && out->kind == PGWT_MERGE_OUT_DATA) {
        ++c->released_data;
        /* Semantic event time to publication: includes kernel-buffer time. */
        if (post->d >= out->record.t) {
            uint64_t before = c->event_max;
            age_add(c->event_hist, &c->event_max, post->d - out->record.t);
            if (c->event_max > before) {
                /* Which record it was: its kind explains a large maximum. */
                c->event_max_rank = (unsigned)out->record.rank;
                c->event_max_source = (unsigned)out->record.source;
                c->event_max_pid = out->record.pid;
            }
        }
    }
    if (ok && out->kind == PGWT_MERGE_OUT_TEXT) ++c->released_text;
    if (ok && out->kind == PGWT_MERGE_OUT_FRONTIER) {
        ++c->frontiers;
        if (c->pause_open && c->rearms && out->F >= c->rearm_at) {
            /* The new FRONTIER certifies only from the fresh proof onward.
             * Confirm that lower bound now; never bridge the lost interval. */
            c->pause_end = c->rearm_at;
            c->pause_ns += c->pause_end - c->pause_start;
            c->covered_ns += out->F - c->pause_end;
            c->pause_open = false;
            fprintf(stderr, "exact-perf: capacity_resume start_ns=%" PRIu64 " end_ns=%" PRIu64
                " reason=%s pool=%s dropped=%" PRIu64 " attempts=%" PRIu64 "\n",
                c->pause_start, c->pause_end, pgwt_merge_reason_name(c->pause_reason),
                pgwt_pool_name(c->pause_pool), c->dropped_records, c->recovery_attempts);
        } else if (!c->pause_open && out->F > c->coverage_cursor) {
            c->covered_ns += out->F - c->coverage_cursor;
        }
        if (out->F > c->coverage_cursor) c->coverage_cursor = out->F;
    }
    if (out->kind == PGWT_MERGE_OUT_VOID) {
        ++c->voids;
        if (out->reason == PGWT_MERGE_ALLOCATION_FAIL) c->diagnostics.allocation = out->allocation;
    }
    if (out->kind == PGWT_MERGE_OUT_VOID) {
        held_age(c, out->d);
        c->dropped_records += c->arrival_count;
        arrival_reset(c);
        bool capacity = ok && pgwt_transient_capacity(out->reason, out->allocation.pool);
        c->capacity_paused = capacity;
        c->backlog_discarded = false;
        if (capacity) {
            ++c->pauses;
            if (!c->pause_open) c->pause_start = c->coverage_cursor;
            c->pause_end = 0;
            c->pause_open = true;
            c->pause_reason = out->reason; c->pause_pool = out->allocation.pool;
            fprintf(stderr, "exact-perf: capacity_pause count=%" PRIu64 " start_ns=%" PRIu64
                " detected_ns=%" PRIu64 " reason=%s pool=%s dropped=%" PRIu64 "\n",
                c->pauses, c->pause_start, out->d, pgwt_merge_reason_name(out->reason),
                pgwt_pool_name(out->allocation.pool), c->dropped_records);
        }
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
    retain_snapshot(c);
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
    c->config = *cfg;
    c->capture_start = c->coverage_cursor = cfg->baseline.d;
    for (pgwt_pool pool = PGWT_POOL_CAPTURE_ARRIVAL; pool <= PGWT_POOL_CAPTURE_RETAINED; ++pool)
        c->diagnostics.pools[pool].capacity = cfg->capacity;
    c->source_count = cfg->cpu_count + 2;
    c->arrival_positions = calloc(c->source_count, sizeof(*c->arrival_positions));
    c->arrival_promises = calloc(c->source_count, sizeof(*c->arrival_promises));
    c->observation = cfg->baseline;
    retain_snapshot(c);
    c->last_checkpoint_at = cfg->baseline.d;
    c->lifetimes = calloc(cfg->capacity, sizeof(*c->lifetimes));
    c->fresh_proofs = calloc(cfg->capacity, sizeof(*c->fresh_proofs));
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
    if (!c->arrival_promises || !c->fresh_proofs || !c->lifetime_index || !c->binding_index || !c->arrival_positions || !c->arrivals || !c->lifetimes || !c->bindings || !c->held || !c->heads || !c->tails || !c->storage || !sources)
        goto fail;
    arrival_reset(c);
    for (size_t i = 0; i < cfg->capacity; ++i) {
        c->storage[i].next = c->free; c->free = &c->storage[i];
    }
    for (size_t i = 0; i < c->source_count; ++i)
        sources[i] = (ma_source_decl){.mechanism = i < cfg->cpu_count ? MA_WATCHPOINT_PERF : MA_ORDERED_RING,
            .qualification_complete = true, .initial_cutoff = initial ? initial->sources[i].cutoff : 0};
    for (size_t i = 0; i < c->source_count; ++i)
        c->arrival_promises[i] = sources[i].initial_cutoff;
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
    pgwt_pool_set(&c->diagnostics.pools[PGWT_POOL_CAPTURE_LIFETIME], c->lifetime_count);
    pgwt_pool_set(&c->diagnostics.pools[PGWT_POOL_CAPTURE_BINDING], c->binding_count);
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
pgwt_merge_diagnostics pgwt_capture_diagnostics(const struct pgwt_merge_capture *c)
{
    if (!c) return (pgwt_merge_diagnostics){0};
    pgwt_merge_diagnostics d = pgwt_merge_get_diagnostics(c->core);
    for (pgwt_pool p = PGWT_POOL_CAPTURE_ARRIVAL; p <= PGWT_POOL_CAPTURE_RETAINED; ++p)
        d.pools[p] = c->diagnostics.pools[p];
    d.pools[PGWT_POOL_CAPTURE_ARRIVAL].occupancy = c->arrival_count;
    d.pools[PGWT_POOL_CAPTURE_LIFETIME].occupancy = c->lifetime_count;
    d.pools[PGWT_POOL_CAPTURE_BINDING].occupancy = c->binding_count;
    d.pools[PGWT_POOL_CAPTURE_RETAINED].occupancy = c->retained_count;
    if (c->diagnostics.allocation.pool) d.allocation = c->diagnostics.allocation;
    return d;
}
static int capture_full(struct pgwt_merge_capture *c, pgwt_pool pool)
{
    pgwt_merge_diagnostics d = pgwt_capture_diagnostics(c);
    c->diagnostics.allocation = (pgwt_allocation_failure){pool, d.pools[pool]};
    return -ENOMEM;
}
int pgwt_capture_allocation_fault(struct pgwt_merge_capture *c, pgwt_allocation_failure failure,
                                   uint64_t d, const ma_snapshot *s)
{
    if (!c) return -EINVAL;
    c->diagnostics.allocation = failure;
    return pgwt_capture_fault(c, PGWT_MERGE_ALLOCATION_FAIL, d, s);
}
int pgwt_capture_fault(struct pgwt_merge_capture *c, pgwt_merge_reason reason,
                        uint64_t d, const ma_snapshot *s)
{
    if (!c || c->finished) return -EINVAL;
    observation(c, d, s);
    retain_snapshot(c);
    pgwt_merge_input in = {.kind = PGWT_MERGE_FAULT, .reason = reason, .observation = c->observation};
    if (reason == PGWT_MERGE_ALLOCATION_FAIL) in.allocation = c->diagnostics.allocation;
    pgwt_merge_status status = pgwt_merge_feed(c->core, &in);
    return status == PGWT_MERGE_EXIT_REQUIRED || pgwt_merge_get_report(c->core).void_failed ? -EIO : 0;
}
/* This is the same explicit premise acquisition used at startup and on
 * recovery. It deliberately has no production fallback: A2 must supply that
 * mechanism. A prior MA_PROVED value is never evidence for this call. */
static int acquire_test_attachment(struct pgwt_merge_capture *c, ma_lifetime *l,
                                     uint64_t now, const ma_snapshot *snapshot)
{
    if (!c->config.test_attach_premise) return 0;
    ++c->proof_acquisitions;
    struct pgwt_attach_report r = {.pid = (uint32_t)l->pid, .cookie = l->g, .event = MA_BEGIN};
    if (pgwt_capture_attach(c, &r, now, snapshot) < 0) return -EIO;
    r.event = MA_COMPLETE; r.facts_known = r.facts_true = MA_ALL_FACTS;
    ma_snapshot snap = pgwt_capture_last_snapshot(c);
    if (pgwt_capture_attach(c, &r, now, &snap) < 0) return -EIO;
    return l->proof == MA_PROVED ? 1 : 0;
}
int pgwt_capture_received_token(struct pgwt_merge_capture *c, const ma_envelope *e, uint64_t now, size_t *token)
{
    if (token) *token = 0;
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
    ++c->payload_records;
    bool uncovered = e->kind != MA_TEXT && !(e->cookie_present && e->cookie);
    bool preproof = (c->rearm_at && e->kind != MA_TEXT &&
        get_le(e->bytes + (e->kind == MA_TRACE ? 0 : 16), 8) < c->rearm_at);
    bool dropped = !uncovered && (pgwt_capture_report(c).status != PGWT_MERGE_OK || preproof);
    /* TEXT has no semantic timestamp. Queued pre-proof TEXT is conservatively
     * not trusted until a post-REARM completed round (the transport fences it). */
    if (uncovered) ++c->uncovered_records;
    else if (dropped) {
        ++c->dropped_records;
        /* Disposition cannot hide DATA disproving an earlier epoch. Only the
         * core decides clock/seal faults and invalidates published frontiers.
         * Route such a record through its ordinary feed, even before fresh
         * boundaries. Other pre-proof records remain counted discards. */
        if (c->rearm_at && pgwt_capture_report(c).status == PGWT_MERGE_OK &&
            e->kind != MA_TEXT && e->source < c->source_count &&
            get_le(e->bytes + (e->kind == MA_TRACE ? 0 : 16), 8) < c->arrival_promises[e->source]) {
            ma_snapshot snap = pgwt_capture_last_snapshot(c);
            if (pgwt_capture_envelope(c, e, now, &snap) < 0) return -EIO;
        }
    }
    if (!uncovered && !dropped) {
        size_t duplicate = e->source < c->source_count &&
            e->position > c->arrival_positions[e->source] ? 0 :
            arrival_find(c, e->source, e->position);
        if (c->arrival_free == SIZE_MAX) {
            ++c->dropped_records; /* this wire record could not get a slot */
            return capture_full(c, PGWT_POOL_CAPTURE_ARRIVAL);
        }
        if (duplicate) {
            ++c->dropped_records;
            ma_snapshot snap = pgwt_capture_last_snapshot(c);
            return pgwt_capture_fault(c, PGWT_MERGE_DUPLICATE_IDENTITY, now, &snap);
        }
        size_t index = c->arrival_free;
        c->arrival_free = c->arrivals[index].next;
        c->arrivals[index] = (struct arrival){.source = e->source, .position = e->position,
            .at = now, .prev = c->newest, .next = SIZE_MAX, .active = true};
        memcpy(c->arrivals[index].prefix, e->bytes, sizeof(uint64_t));
        if (e->source < c->source_count && e->position > c->arrival_positions[e->source])
            c->arrival_positions[e->source] = e->position;
        if (token) *token = index + 1;
        if (c->newest == SIZE_MAX) c->oldest = index;
        else c->arrivals[c->newest].next = index;
        c->newest = index;
        pgwt_pool_set(&c->diagnostics.pools[PGWT_POOL_CAPTURE_ARRIVAL], ++c->arrival_count);
    }
    if (e->kind == MA_LIFECYCLE && get_le(e->bytes, 4) == 1 && e->cookie_present && e->cookie) {
        uint64_t pid = get_le(e->bytes + 4, 4);
        ma_table t = table(c);
        size_t slot = ma_lifetime_bucket(&t, pid, e->cookie, NULL);
        if (!c->lifetime_index[slot]) {
            if (c->lifetime_count == c->config.capacity) return capture_full(c, PGWT_POOL_CAPTURE_LIFETIME);
            c->lifetimes[c->lifetime_count++] = (ma_lifetime){.pid = pid,
                .g = e->cookie, .identity_bound = true, .proof = MA_NOT_ATTEMPTED};
            c->lifetime_index[slot] = c->lifetime_count;
            pgwt_pool_set(&c->diagnostics.pools[PGWT_POOL_CAPTURE_LIFETIME], c->lifetime_count);
            if (c->config.test_attach_premise) {
                ma_snapshot snap = pgwt_capture_last_snapshot(c);
                if (acquire_test_attachment(c, &c->lifetimes[c->lifetime_count - 1], now, &snap) < 0)
                    return -EIO;
            }
        }
    }
    uint64_t query = uncovered ? 0 : e->kind == MA_TRACE ? get_le(e->bytes + 32, 8) :
                     e->kind == MA_TEXT ? get_le(e->bytes + 8, 8) : 0;
    if (query) {
        ma_table t = table(c);
        size_t slot = ma_binding_bucket(&t, query, NULL);
        if (!c->binding_index[slot]) {
            if (c->binding_count == c->config.capacity) return capture_full(c, PGWT_POOL_CAPTURE_BINDING);
            c->bindings[c->binding_count++] = (ma_binding){.query_id = query, .kind = MA_NOT_REQUIRED};
            c->binding_index[slot] = c->binding_count;
            pgwt_pool_set(&c->diagnostics.pools[PGWT_POOL_CAPTURE_BINDING], c->binding_count);
        }
    }
    if ((e->kind == MA_LIFECYCLE || e->kind == MA_TEXT) && c->config.lifecycle) {
        int rc = c->config.lifecycle(c->config.ctx, e->bytes, e->size);
        if (rc < 0) {
            if (c->config.writer_failed) c->config.writer_failed(c->config.ctx);
            else if (c->local_writer_fail != UINT64_MAX) ++c->local_writer_fail;
            retain_snapshot(c); /* local failure may invalidate the borrowed vector */
            return rc;
        }
    }
    /* Preserve the legacy disposition on unrelated permanent suspension. */
    return uncovered ? 1 : dropped && (c->capacity_paused || preproof) ? 2 : 0;
}
int pgwt_capture_received(struct pgwt_merge_capture *c, const ma_envelope *e, uint64_t now)
{
    int rc = pgwt_capture_received_token(c, e, now, NULL);
    return rc == 2 ? 0 : rc; /* legacy callers already suppress while suspended */
}
const ma_snapshot *pgwt_capture_snapshot(const struct pgwt_merge_capture *c)
{ return c ? &c->snapshot : NULL; }
/* Normalize a retained observation only when its vector changes. Publication
 * normally updates just time, so it must not rebuild/copy fifteen counters. */
static void retain_snapshot(struct pgwt_merge_capture *c)
{
    ma_snapshot *s = &c->snapshot;
    bool known = c->observation.counters.known;
    s->valid = known && (c->config.writer_failed ||
        c->observation.counters.value[14] >= c->local_writer_fail);
    if (s->component_valid[0] != known)
        for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i) s->component_valid[i] = known;
    if (memcmp(s->value, c->observation.counters.value, sizeof(s->value)))
        memcpy(s->value, c->observation.counters.value, sizeof(s->value));
    if (!c->config.writer_failed && s->value[14] >= c->local_writer_fail)
        s->value[14] -= c->local_writer_fail;
}
ma_snapshot pgwt_capture_last_snapshot(const struct pgwt_merge_capture *c)
{ return c ? *pgwt_capture_snapshot(c) : (ma_snapshot){0}; }
static int mapped(struct pgwt_merge_capture *c, const ma_envelope *e,
                    size_t token, uint64_t d, const ma_snapshot **s, bool *hold)
{
    ma_table t = table(c);
    static const ma_snapshot unknown;
    const ma_snapshot *snap = *s ? *s : &unknown;
    ma_snapshot adjusted;
    if (!c->config.writer_failed && c->local_writer_fail) {
        adjusted = *snap;
        uint64_t v = adjusted.value[14];
        if (UINT64_MAX - v < c->local_writer_fail) adjusted.valid = false;
        else adjusted.value[14] += c->local_writer_fail;
        snap = &adjusted;
    }
    /* The frozen adapter consumes the original 32-bit lifecycle tag.
     * Strip only EXIT diagnostics in a temporary view; retain the received
     * payload for logging/replay, and leave core/ and its evidence unchanged. */
    ma_envelope semantic;
    const ma_envelope *adapter_input = e;
    unsigned char exit_payload[24];
    if (e->kind == MA_LIFECYCLE && e->size == sizeof(exit_payload) &&
        get_le(e->bytes, 2) == PGWT_LIFECYCLE_EXIT && get_le(e->bytes + 2, 2)) {
        memcpy(exit_payload, e->bytes, sizeof(exit_payload));
        exit_payload[2] = exit_payload[3] = 0;
        semantic = *e;
        semantic.bytes = exit_payload;
        adapter_input = &semantic;
    }
    ma_result r = ma_map(c->adapter, adapter_input, &t, d, snap);
    if (adapter_input != e) {
        for (size_t i = 0; i < r.count; ++i)
            if (r.input[i].kind == PGWT_MERGE_DATA) r.input[i].record.payload = e->bytes;
    }
    /* This is the observation we retain across callbacks. ma_map already
     * assembled it; no second snapshot/observation conversion is needed. */
    c->observation = r.input[0].observation;
    retain_snapshot(c);
    *hold = r.hold;
    for (size_t i = 0; i < r.count; ++i) {
        if (i) r.input[i].observation = c->observation;
        unsigned char payload[272];
        if (r.input[i].kind == PGWT_MERGE_DATA || r.input[i].kind == PGWT_MERGE_TEXT) {
            ++c->staged;
            /* The core treats payload as opaque bytes. Substitute the first
             * eight bytes with our slot token AFTER mapping; restore before
             * any consumer sees it. Size/capacity and all core rules stay the
             * same, including TEXT's 272-byte payload limit. */
            if (token && token <= c->config.capacity && c->arrivals[token - 1].active &&
                c->arrivals[token - 1].source == r.input[i].record.source &&
                c->arrivals[token - 1].position == r.input[i].record.o) {
                memcpy(payload, r.input[i].record.payload, r.input[i].record.payload_size);
                uint64_t id = token;
                memcpy(payload, &id, sizeof(id));
                r.input[i].record.payload = payload;
            }
        }
        if (pgwt_merge_feed(c->core, &r.input[i]) == PGWT_MERGE_EXIT_REQUIRED ||
            pgwt_merge_get_report(c->core).void_failed) return -EIO;
        if (r.input[i].kind == PGWT_MERGE_BOUNDARY) {
            size_t source = r.input[i].source;
            if (r.input[i].w > c->arrival_promises[source]) c->arrival_promises[source] = r.input[i].w;
        } else if (r.input[i].kind == PGWT_MERGE_DATA && r.input[i].record.identity_known &&
                   r.input[i].record.source < c->config.cpu_count) {
            size_t source = r.input[i].record.source;
            if (r.input[i].record.t > c->arrival_promises[source])
                c->arrival_promises[source] = r.input[i].record.t;
        }
        if (flush_outputs(c) < 0) return -EIO;
    }
    if (r.has_lifetime_update) {
        size_t slot = ma_lifetime_bucket(&t, r.next_lifetime.pid, r.next_lifetime.g, NULL);
        if (c->lifetime_index[slot])
            c->lifetimes[c->lifetime_index[slot] - 1] = r.next_lifetime;
    }
    *s = &c->snapshot;
    return 0;
}
static int flush_source(struct pgwt_merge_capture *c, size_t source,
                         uint64_t d, const ma_snapshot **s, bool observation_delivered)
{
    while (c->heads[source]) {
        if (c->config.now && pgwt_budget_expired(&c->budget, c->config.now(c->config.ctx))) {
            pgwt_budget_yield(&c->budget);
            ++c->source_budget_yields;
            /* A control input already delivered this observation. Otherwise
             * defer the record only after delivering its caller's vector. */
            if (observation_delivered) return 0;
            ma_envelope check = {.version = 1, .kind = MA_CHECK};
            bool hold;
            return mapped(c, &check, 0, d, s, &hold);
        }
        struct retained *n = c->heads[source];
        bool hold;
        int rc = mapped(c, &n->envelope, n->token, d, s, &hold);
        if (rc < 0) return rc;
        observation_delivered = true;
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
void pgwt_capture_set_budget(struct pgwt_merge_capture *c, const struct pgwt_transport_budget *b)
{ if (c && b) c->budget = *b; }
int pgwt_capture_feed(struct pgwt_merge_capture *c, const ma_envelope *e,
                           size_t token, uint64_t d, const ma_snapshot **snapshot)
{
    if (!c || !e || c->finished || !snapshot) return -EINVAL;
    const ma_snapshot *s = *snapshot;
    const ma_snapshot *current = s;
    *snapshot = &c->snapshot;
    /* Control inputs carry observations: deliver them before yielding any
     * retained work. In particular a fresh CHECK must never disappear at
     * a completion fence or periodic checkpoint. */
    if (e->kind != MA_TRACE && e->kind != MA_LIFECYCLE && e->kind != MA_TEXT && e->kind != MA_BOUNDARY) {
        bool hold;
        int rc = mapped(c, e, 0, d, &current, &hold);
        if (rc < 0) return rc;
        for (size_t visited = 0; visited < c->source_count; ++visited) {
            if (c->config.now && pgwt_budget_expired(&c->budget, c->config.now(c->config.ctx))) {
                if (c->retained_count) pgwt_budget_yield(&c->budget);
                ++c->sweep_budget_yields;
                break;
            }
            size_t i = c->next_source;
            c->next_source = (i + 1) % c->source_count;
            if ((rc = flush_source(c, i, d, &current, true)) < 0) return rc;
        }
        return 0;
    }
    if (e->source >= c->source_count || e->size > 272 || (e->size && !e->bytes))
        return pgwt_capture_fault(c, PGWT_MERGE_MALFORMED_ENVELOPE, d, s);
    if (!c->free) {
        capture_full(c, PGWT_POOL_CAPTURE_RETAINED);
        return pgwt_capture_fault(c, PGWT_MERGE_ALLOCATION_FAIL, d, s);
    }
    struct retained *n = c->free; c->free = n->next; n->next = NULL;
    n->envelope = *e;
    n->token = token;
    if (e->size) { memcpy(n->bytes, e->bytes, e->size); n->envelope.bytes = n->bytes; }
    if (c->tails[e->source]) c->tails[e->source]->next = n;
    else c->heads[e->source] = n;
    c->tails[e->source] = n; ++c->retained_count;
    pgwt_pool_set(&c->diagnostics.pools[PGWT_POOL_CAPTURE_RETAINED], c->retained_count);
    return flush_source(c, e->source, d, &current, false);
}
int pgwt_capture_envelope_token(struct pgwt_merge_capture *c, const ma_envelope *e,
                           size_t token, uint64_t d, const ma_snapshot *s)
{ return pgwt_capture_feed(c, e, token, d, &s); }
int pgwt_capture_envelope(struct pgwt_merge_capture *c, const ma_envelope *e,
                           uint64_t d, const ma_snapshot *s)
{
    /* Compatibility entry for untagged injectable callers. Production uses
     * the token entry directly, with no source/position lookup. */
    bool wire = e && (e->kind == MA_TRACE || e->kind == MA_LIFECYCLE || e->kind == MA_TEXT);
    size_t token = c && wire ? arrival_find(c, e->source, e->position) : 0;
    return pgwt_capture_envelope_token(c, e, token, d, s);
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
    /* The final vector is mandatory even at the last pass's budget edge.
     * Feed it directly; any retained suffix is unfinished evidence below,
     * rather than another potentially unbounded drain at shutdown. */
    c->unfinished_records = pgwt_capture_pending_records(c);
    const ma_snapshot *current = s;
    bool hold;
    int rc = mapped(c, &check, 0, d, &current, &hold);
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
    free(c->fresh_proofs); free(c->storage); free(c->arrivals); free(c->arrival_positions); free(c->arrival_promises); free(c);
}
bool pgwt_capture_capacity_paused(const struct pgwt_merge_capture *c)
{ return c && !c->finished && c->capacity_paused && pgwt_capture_report(c).status == PGWT_MERGE_SUSPENDED; }
void pgwt_capture_discard_backlog(struct pgwt_merge_capture *c)
{
    if (!pgwt_capture_capacity_paused(c)) return;
    /* Arrival tokens were counted and revoked at VOID. Rebuild the retained
     * free list only outside a callback, when no feed owns one of its nodes. */
    c->dropped_records += c->arrival_count;
    arrival_reset(c);
    for (size_t i = 0; i < c->source_count; ++i)
        for (struct retained *n = c->heads[i]; n; n = n->next)
            if (n->envelope.kind == MA_BOUNDARY) ++c->dropped_boundaries;
    memset(c->held, 0, c->source_count * sizeof(*c->held));
    memset(c->heads, 0, c->source_count * sizeof(*c->heads));
    memset(c->tails, 0, c->source_count * sizeof(*c->tails));
    c->free = NULL;
    for (size_t i = 0; i < c->config.capacity; ++i) {
        c->storage[i].next = c->free; c->free = &c->storage[i];
    }
    c->retained_count = c->staged = 0;
    /* All adapter proofs belong to the discarded epoch. Preserve identities
     * and binding history, but reacquire facts rather than copying them. */
    for (size_t i = 0; i < c->lifetime_count; ++i) {
        c->lifetimes[i].proof = MA_NOT_ATTEMPTED;
        c->lifetimes[i].facts_known = c->lifetimes[i].facts_true = 0;
        c->lifetimes[i].fork_forwarded = c->lifetimes[i].failure_reported = false;
    }
    c->backlog_discarded = true;
}
int pgwt_capture_rearm(struct pgwt_merge_capture *c, uint64_t now, const ma_snapshot *s)
{
    if (!pgwt_capture_capacity_paused(c)) return 0;
    ++c->recovery_attempts;
    fprintf(stderr, "exact-perf: recovery_attempt count=%" PRIu64 " at_ns=%" PRIu64
        " pool=%s\n", c->recovery_attempts, now, pgwt_pool_name(c->pause_pool));
    /* Every observed vector reaches CHECK, including unreadable observations
     * during suspension. counter_unknown forbids recovery just as before. */
    ma_envelope check = {.version = 1, .kind = MA_CHECK};
    if (pgwt_capture_envelope(c, &check, now, s) < 0) return -EIO;
    if (!pgwt_capture_capacity_paused(c)) return 0;
    if (!c->backlog_discarded || !s || !s->valid) return 0;
    for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i)
        if (!s->component_valid[i]) return 0;
    if (!c->config.test_attach_premise) return 0; /* production A2 pending */
    for (size_t i = 0; i < c->lifetime_count; ++i) {
        ma_lifetime *l = &c->lifetimes[i];
        /* Every attempt obtains new reports, even after an incomplete try. */
        l->proof = MA_NOT_ATTEMPTED; l->fork_forwarded = false;
        l->facts_known = l->facts_true = 0;
        int acquired = acquire_test_attachment(c, l, now, s);
        if (acquired <= 0) return acquired;
        if (!pgwt_capture_capacity_paused(c)) return 0; /* acquisition observed another fault */
        c->fresh_proofs[i] = (pgwt_merge_attachment){.pid = l->pid, .g = l->g,
            .identity_proved = true}; /* N: no sequence allocator */
    }
    /* Acquisition can be preempted or run callbacks. The coverage lower
     * bound is its completion time, never the attempt's earlier start. */
    uint64_t began = now;
    if (c->config.now) now = c->config.now(c->config.ctx);
    if (now < began) return 0;
    observation(c, now, s);
    pgwt_merge_input in = {.kind = PGWT_MERGE_REARM, .observation = c->observation,
        .attachments = c->fresh_proofs, .attachment_count = c->lifetime_count,
        .attachment_proofs_fresh = true}; /* only after fresh acquisition above */
    pgwt_merge_status status = pgwt_merge_feed(c->core, &in);
    retain_snapshot(c);
    if (status == PGWT_MERGE_EXIT_REQUIRED || pgwt_capture_report(c).void_failed) return -EIO;
    if (status != PGWT_MERGE_OK) return 0;
    c->capacity_paused = false;
    c->rearm_at = now; ++c->rearms;
    fprintf(stderr, "exact-perf: REARM epoch=%" PRIu64 " at_ns=%" PRIu64
        "; waiting for fresh qualified boundaries (TEST PREMISE)\n", pgwt_capture_report(c).epoch, now);
    return 1;
}
struct pgwt_capture_coverage pgwt_capture_coverage(const struct pgwt_merge_capture *c, uint64_t now)
{
    struct pgwt_capture_coverage s = {0};
    if (!c) return s;
    s.pauses = c->pauses; s.attempts = c->recovery_attempts;
    s.proof_acquisitions = c->proof_acquisitions; s.rearms = c->rearms;
    s.last_start = c->pause_start; s.last_end = c->pause_end;
    s.open = c->pause_open; s.reason = c->pause_reason; s.pool = c->pause_pool;
    s.pause_ns = c->pause_ns + (c->pause_open && now >= c->pause_start ? now - c->pause_start : 0);
    s.window_ns = now >= c->capture_start ? now - c->capture_start : 0;
    s.epoch_invalid = pgwt_capture_report(c).epoch_invalid;
    /* Invalidated prefixes are unavailable, not still advertised as covered.
     * This run-level report conservatively withdraws all coverage; it is not
     * a replacement for the core's per-epoch invalidation history. */
    s.covered_ns = s.epoch_invalid ? 0 : c->covered_ns;
    /* Independently sum recorded pause intervals and the uncertified tail.
     * An open pause already includes that tail, so never count it twice.
     * Do not clamp covered time: an accounting error must break the identity. */
    s.unavailable_ns = s.pause_ns;
    if (!c->pause_open && now >= c->coverage_cursor)
        s.unavailable_ns += now - c->coverage_cursor;
    if (s.epoch_invalid) s.unavailable_ns = s.window_ns;
    s.payload_records = c->payload_records; s.dropped = c->dropped_records;
    s.uncovered = c->uncovered_records; s.pending = c->arrival_count;
    s.dropped_boundaries = c->dropped_boundaries;
    return s;
}
void pgwt_capture_coverage_print(FILE *out, const struct pgwt_merge_capture *c, uint64_t now,
                                  uint64_t wire, uint64_t controls, uint64_t rejected, uint64_t dropped_controls)
{
    struct pgwt_capture_coverage s = pgwt_capture_coverage(c, now);
    uint64_t released = c->released_data + c->released_text;
    controls -= s.dropped_boundaries;
    uint64_t dropped = s.dropped + s.dropped_boundaries + dropped_controls;
    uint64_t accounted = released + dropped + s.uncovered + s.pending + controls + rejected;
    fprintf(out, "exact-perf: coverage capacity_pauses=%" PRIu64 " recovery_attempts=%" PRIu64
        " rearms=%" PRIu64 " pause_seconds=%.9f unavailable_seconds=%.9f last_start_ns=%" PRIu64 " last_end_ns=%" PRIu64
        " open=%d reason=%s pool=%s dropped=%" PRIu64 " window_ns=%" PRIu64
        " covered_ns=%" PRIu64 " unavailable_ns=%" PRIu64 " epoch_invalid=%d time_identity=%d\n",
        s.pauses, s.attempts, s.rearms, (double)s.pause_ns / 1e9, (double)s.unavailable_ns / 1e9,
        s.last_start, s.last_end,
        s.open, s.pauses ? pgwt_merge_reason_name(s.reason) : "none", pgwt_pool_name(s.pool), dropped,
        s.window_ns, s.covered_ns, s.unavailable_ns, s.epoch_invalid,
        s.covered_ns + s.unavailable_ns == s.window_ns);
    fprintf(out, "exact-perf: wire_identity wire=%" PRIu64 " released=%" PRIu64
        " counted_dropped=%" PRIu64 " uncovered=%" PRIu64 " pending=%" PRIu64
        " boundary_control=%" PRIu64 " rejected_fault=%" PRIu64 " ok=%d\n",
        wire, released, dropped, s.uncovered, s.pending, controls, rejected, wire == accounted);
}
struct pgwt_capture_stats pgwt_capture_stats(const struct pgwt_merge_capture *c)
{
    struct pgwt_capture_stats s = {0};
    if (!c) return s;
    s.source_budget_yields = c->source_budget_yields;
    s.sweep_budget_yields = c->sweep_budget_yields;
    s.unfinished_records = c->unfinished_records;
    s.released_data = c->released_data; s.released_text = c->released_text;
    s.frontiers = c->frontiers; s.voids = c->voids;
    s.held_max_ns = c->held_max; s.event_max_ns = c->event_max;
    s.held_p50_ns = age_quantile(c->held_hist, 5000); s.held_p99_ns = age_quantile(c->held_hist, 9900);
    s.event_p50_ns = age_quantile(c->event_hist, 5000); s.event_p99_ns = age_quantile(c->event_hist, 9900);
    s.event_max_rank = c->event_max_rank; s.event_max_source = c->event_max_source;
    s.event_max_pid = c->event_max_pid;
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
static uint64_t daemon_now(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
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
        pgwt_suspension_print(stderr, out);
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
        .trace = daemon_trace, .observe = daemon_observe, .now = daemon_now, .writer_failed = daemon_writer_failed,
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
    /* Decide cadence before reading any counter component. An observation
     * already taken must always reach CHECK, even when the pass expired. */
    uint64_t now = daemon_now(d);
    if (now && !pgwt_transport_service_due(now, d->merge_capture->last_service_at,
                                         d->merge_capture->serviced)) return 0;
    ma_snapshot s = pgwt_capture_last_snapshot(d->merge_capture);
    pgwt_merge_observation o = d->merge_capture->observation;
    int rc = daemon_observe(d, &o);
    now = o.d;
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
    pgwt_percpu_transport_finish_diagnostics(d);
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
    if (ts.finish_deadline_reached || ts.finish_pass_limit_reached || ts.unfinished_records)
        fprintf(stderr, "exact-perf: capture ended at deadline/stop with unfinished evidence; "
            "finish_deadline_reached=%llu finish_pass_limit_reached=%llu unfinished_records=%llu; VOID, not complete\n",
            (unsigned long long)ts.finish_deadline_reached, (unsigned long long)ts.finish_pass_limit_reached,
            (unsigned long long)ts.unfinished_records);
#define U(v) ((unsigned long long)(v))
    fprintf(stderr, "exact-perf: stats rounds_requested=%llu rounds_completed=%llu "
        "rounds_completed_nonempty=%llu holds=%llu cuts=%llu terminal_discards=%llu skipped=%llu "
        "uncovered=%llu wire_records=%llu queue_high=%llu round_us_mean=%llu round_us_max=%llu "
        "cpus=%llu acks_at_completion=%llu acks_before_fence=%llu "
        "released_data=%llu released_text=%llu frontiers=%llu voids=%llu "
        "held_us_p50=%llu held_us_p99=%llu held_us_max=%llu "
        "event_us_p50=%llu event_us_p99=%llu event_us_max=%llu "
        "event_max_rank=%u event_max_source=%u event_max_pid=%llu "
        "seal_violation=%llu sequence_gap=%llu unknown_evidence=%llu perf_lost=%llu "
        "finish_deadline_reached=%llu finish_pass_limit_reached=%llu unfinished_records=%llu status=%u\n",
        U(ts.rounds_requested), U(ts.rounds_completed), U(ts.rounds_completed_nonempty),
        U(ts.holds), U(ts.cuts), U(ts.terminal_discards), U(ts.skipped), U(ts.uncovered),
        U(ts.records), U(ts.queue_high),
        U(ts.rounds_completed ? ts.round_ns_total / ts.rounds_completed / 1000 : 0),
        U(ts.round_ns_max / 1000), U(pgwt_percpu_transport_cpu_count(d)),
        U(ts.acks_at_completion), U(ts.acks_before_fence), U(cs.released_data), U(cs.released_text),
        U(cs.frontiers), U(cs.voids), U(cs.held_p50_ns / 1000), U(cs.held_p99_ns / 1000),
        U(cs.held_max_ns / 1000), U(cs.event_p50_ns / 1000), U(cs.event_p99_ns / 1000),
        U(cs.event_max_ns / 1000), cs.event_max_rank, cs.event_max_source, U(cs.event_max_pid),
        U(report.faults[PGWT_MERGE_SEAL_VIOLATION]),
        U(report.faults[PGWT_MERGE_SEQUENCE_GAP]), U(report.faults[PGWT_MERGE_UNKNOWN_EVIDENCE]),
        U(lost), U(ts.finish_deadline_reached), U(ts.finish_pass_limit_reached), U(ts.unfinished_records), (unsigned)report.status);
    fprintf(stderr, "exact-perf: budgets drain_budget_yields=%llu drain_slice_yields=%llu release_budget_yields=%llu "
        "reader_budget_yields=%llu ring_budget_yields=%llu perf_budget_yields=%llu hint_budget_yields=%llu "
        "consume_budget_yields=%llu wait_budget_yields=%llu ring_record_cap_yields=%llu cut_pass_limit_reached=%llu cut_pass_limit_records=%llu "
        "source_budget_yields=%llu sweep_budget_yields=%llu unfinished_transport_records=%llu "
        "unfinished_capture_records=%llu unfinished_unread_records=%llu capture_final_unfinished_records=%llu\n",
        U(ts.drain_budget_yields), U(ts.drain_slice_yields), U(ts.release_budget_yields), U(ts.reader_budget_yields),
        U(ts.ring_budget_yields), U(ts.perf_budget_yields), U(ts.hint_budget_yields),
        U(ts.consume_budget_yields), U(ts.wait_budget_yields), U(ts.ring_record_cap_yields), U(ts.cut_pass_limit_reached), U(ts.cut_pass_limit_records),
        U(cs.source_budget_yields), U(cs.sweep_budget_yields), U(ts.unfinished_transport_records),
        U(ts.unfinished_capture_records), U(ts.unfinished_unread_records), U(cs.unfinished_records));
    pgwt_capture_coverage_print(stderr, d->merge_capture, now, ts.records, ts.controls, ts.rejected, ts.dropped_controls);
    pgwt_merge_diagnostics diagnostics = pgwt_capture_diagnostics(d->merge_capture);
    diagnostics.pools[PGWT_POOL_TRANSPORT_QUEUE] = ts.queue;
    pgwt_allocation_failure allocation = diagnostics.allocation.pool ? diagnostics.allocation : ts.allocation;
    fprintf(stderr, "exact-perf: allocation_last");
    pgwt_pool_print(stderr, allocation.pool, allocation.usage);
    fputc('\n', stderr);
    for (pgwt_pool pool = PGWT_POOL_TRANSPORT_QUEUE; pool < PGWT_POOL_CORE_SETUP; ++pool) {
        fprintf(stderr, "exact-perf: pool_stats");
        pgwt_pool_print(stderr, pool, diagnostics.pools[pool]);
        fputc('\n', stderr);
    }
    pgwt_distribution_print(stderr, "request_to_el", &ts.request_to_el, false);
    pgwt_distribution_print(stderr, "fence_to_ack", &ts.fence_to_ack, false);
    pgwt_distribution_print(stderr, "ack_to_release_end", &ts.ack_to_release_end, false);
    pgwt_distribution_print(stderr, "release_replay", &ts.release_replay, false);
    pgwt_distribution_print(stderr, "records_per_round", &ts.records_per_round, true);
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
