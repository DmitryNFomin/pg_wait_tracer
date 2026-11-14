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
    FILE *dump;
    ma *adapter;
    pgwt_merge *core;
    struct pgwt_capture_config config;
    ma_lifetime *lifetimes;
    ma_binding *bindings;
    const ma_envelope **held;
    struct retained **heads, **tails, *storage, *free;
    size_t lifetime_count, binding_count, source_count, retained_count;
    size_t staged;
    uint64_t local_writer_fail, max_held;
    struct arrival *arrivals;
    size_t arrival_slots, oldest, newest, arrival_free;
    size_t *arrival_buckets;
    pgwt_merge_observation observation;
    bool finished;
};
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
    if (c->config.observe) refresh(c, post);
    if (!ok) {
        /* The existing trace handler already counts its writer rejection.
         * Count a failing additional consumer only if it has not done so. */
        if (c->config.writer_failed) {
            if (!post->counters.known || post->counters.value[14] == before) {
                c->config.writer_failed(c->config.ctx);
                if (c->config.observe) refresh(c, post);
            }
        } else {
            if (c->local_writer_fail == UINT64_MAX || post->counters.value[14] == UINT64_MAX)
                post->counters.known = false;
            else { ++c->local_writer_fail; ++post->counters.value[14]; }
        }
    }
    c->observation = *post;
    if (ok && (out->kind == PGWT_MERGE_OUT_DATA || out->kind == PGWT_MERGE_OUT_TEXT))
        arrival_remove(c, &out->record, post->d);
    if (out->kind == PGWT_MERGE_OUT_VOID) {
        held_age(c, out->d);
        arrival_reset(c);
    }
    if (ok && (out->kind == PGWT_MERGE_OUT_DATA || out->kind == PGWT_MERGE_OUT_TEXT) && c->staged)
        --c->staged;
    if (out->kind == PGWT_MERGE_OUT_VOID) c->staged = 0;
    return ok;
}

struct pgwt_merge_capture *pgwt_capture_create(const struct pgwt_capture_config *cfg)
{
    if (!cfg || !cfg->cpu_count || cfg->cpu_count > 65536 || !cfg->capacity ||
        cfg->capacity > SIZE_MAX / sizeof(struct retained) ||
        cfg->capacity > SIZE_MAX / 2 / sizeof(struct arrival)) return NULL;
    struct pgwt_merge_capture *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->oldest = c->newest = SIZE_MAX;
    c->arrival_slots = cfg->capacity * 2;
    c->arrivals = calloc(cfg->capacity, sizeof(*c->arrivals));
    c->arrival_buckets = calloc(c->arrival_slots, sizeof(*c->arrival_buckets));
    c->config = *cfg; c->source_count = cfg->cpu_count + 2;
    c->observation = cfg->baseline;
    c->lifetimes = calloc(cfg->capacity, sizeof(*c->lifetimes));
    c->bindings = calloc(cfg->capacity, sizeof(*c->bindings));
    c->held = calloc(c->source_count, sizeof(*c->held));
    c->heads = calloc(c->source_count, sizeof(*c->heads));
    c->tails = calloc(c->source_count, sizeof(*c->tails));
    c->storage = calloc(cfg->capacity, sizeof(*c->storage));
    ma_source_decl *sources = calloc(c->source_count, sizeof(*sources));
    if (!c->arrival_buckets || !c->arrivals || !c->lifetimes || !c->bindings || !c->held || !c->heads || !c->tails || !c->storage || !sources)
        goto fail;
    arrival_reset(c);
    for (size_t i = 0; i < cfg->capacity; ++i) {
        c->storage[i].next = c->free; c->free = &c->storage[i];
    }
    for (size_t i = 0; i < c->source_count; ++i)
        sources[i] = (ma_source_decl){.mechanism = i < cfg->cpu_count ? MA_WATCHPOINT_PERF : MA_ORDERED_RING,
                                      .qualification_complete = true};
    ma_config mc = {.mode = PGWT_MERGE_N, .synthetic_premises = false,
                   .sources = sources, .source_count = c->source_count};
    c->adapter = ma_create_production(&mc, NULL);
    free(sources); sources = NULL;
    if (!c->adapter) goto fail;
    pgwt_merge_config base = {.staged_capacity = cfg->capacity, .payload_capacity = 272,
        .attachment_capacity = cfg->capacity, .frontier_capacity = cfg->capacity,
        .dependency_capacity = cfg->capacity, .sink = capture_sink, .sink_context = c,
        .observation = cfg->baseline, .last_observed = cfg->baseline.counters}, core_config;
    if (!ma_core_config(c->adapter, &base, &core_config)) goto fail;
    pgwt_merge_status status;
    c->core = pgwt_merge_create(&core_config, &status);
    if (!c->core || pgwt_merge_get_report(c->core).void_failed) goto fail;
    return c;
fail:
    free(sources); pgwt_capture_destroy(c); return NULL;
}
static ma_table table(struct pgwt_merge_capture *c)
{
    return (ma_table){.lifetimes = c->lifetimes, .lifetime_count = c->lifetime_count,
        .bindings = c->bindings, .binding_count = c->binding_count,
        .held = c->held, .source_count = c->source_count};
}
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
    if (pgwt_capture_report(c).status == PGWT_MERGE_OK) {
        size_t slot = arrival_slot(c, e->source, e->position);
        if (c->arrival_free == SIZE_MAX) return -ENOMEM;
        if (c->arrival_buckets[slot]) return -EINVAL;
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
        bool found = false;
        for (size_t i = 0; i < c->lifetime_count; ++i)
            if (c->lifetimes[i].pid == pid && c->lifetimes[i].g == e->cookie) found = true;
        if (!found) {
            if (c->lifetime_count == c->config.capacity) return -ENOMEM;
            c->lifetimes[c->lifetime_count++] = (ma_lifetime){.pid = pid,
                .g = e->cookie, .identity_bound = true, .proof = MA_NOT_ATTEMPTED};
        }
    }
    uint64_t query = e->kind == MA_TRACE ? get_le(e->bytes + 32, 8) :
                     e->kind == MA_TEXT ? get_le(e->bytes + 8, 8) : 0;
    if (query) {
        bool found = false;
        for (size_t i = 0; i < c->binding_count; ++i)
            if (c->bindings[i].query_id == query) found = true;
        if (!found) {
            if (c->binding_count == c->config.capacity) return -ENOMEM;
            c->bindings[c->binding_count++] = (ma_binding){.query_id = query, .kind = MA_NOT_REQUIRED};
        }
    }
    if ((e->kind == MA_LIFECYCLE || e->kind == MA_TEXT) && c->config.lifecycle) {
        int rc = c->config.lifecycle(c->config.ctx, e->bytes, e->size);
        if (rc < 0) {
            if (c->config.writer_failed) c->config.writer_failed(c->config.ctx);
            else if (c->local_writer_fail != UINT64_MAX) ++c->local_writer_fail;
        }
        return rc;
    }
    return 0;
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
    }
    if (r.has_lifetime_update) for (size_t i = 0; i < c->lifetime_count; ++i)
        if (c->lifetimes[i].pid == r.next_lifetime.pid && c->lifetimes[i].g == r.next_lifetime.g)
            c->lifetimes[i] = r.next_lifetime;
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
pgwt_merge_report pgwt_capture_report(const struct pgwt_merge_capture *c)
{ return pgwt_merge_get_report(c ? c->core : NULL); }
int pgwt_capture_finish(struct pgwt_merge_capture *c, uint64_t d, const ma_snapshot *s)
{
    if (!c) return -EINVAL;
    if (c->finished) return 0;
    int rc = 0;
    if (pgwt_capture_waiting(c)) rc = pgwt_capture_fault(c, PGWT_MERGE_UNKNOWN_EVIDENCE, d, s);
    c->finished = true;
    return rc;
}
void pgwt_capture_destroy(struct pgwt_merge_capture *c)
{
    if (!c) return;
    pgwt_merge_destroy(c->core); ma_destroy(c->adapter);
    free(c->lifetimes); free(c->bindings); free(c->held); free(c->heads); free(c->tails);
    free(c->storage); free(c->arrivals); free(c->arrival_buckets); free(c);
}
bool pgwt_capture_dump(void *ctx, const pgwt_merge_output *out, pgwt_merge_observation *post)
{
    (void)post;
    FILE *f = ctx;
    if (!f || !out || (unsigned)out->kind > PGWT_MERGE_OUT_VOID ||
        (out->record.payload_size && !out->record.payload)) return false;
    const char *reason = out->kind == PGWT_MERGE_OUT_VOID ? pgwt_merge_reason_name(out->reason) : "";
    if (!reason) return false;
    const char *kind = out->kind == PGWT_MERGE_OUT_DATA ? "DATA" :
        out->kind == PGWT_MERGE_OUT_TEXT ? "TEXT" : out->kind == PGWT_MERGE_OUT_FRONTIER ? "FRONTIER" : "VOID";
    if (fprintf(f, "{\"kind\":\"%s\",\"epoch\":%" PRIu64 ",\"F\":%" PRIu64
        ",\"d\":%" PRIu64 ",\"reason\":\"%s\",\"known\":%s,\"counters\":[",
        kind, out->epoch, out->F, out->d, reason,
        out->counters.known ? "true" : "false") < 0) return false;
    for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i)
        if (fprintf(f, "%s%" PRIu64, i ? "," : "", out->counters.value[i]) < 0) return false;
    const pgwt_merge_record *r = &out->record;
    if (fprintf(f, "],\"prior_interval_certified\":%s,\"source\":%zu,\"o\":%" PRIu64
        ",\"t\":%" PRIu64 ",\"pid\":%" PRIu64 ",\"g\":%" PRIu64 ",\"q\":%" PRIu64
        ",\"rank\":%u,\"identity_known\":%s,\"fork_seed_proved\":%s,\"has_q\":%s,\"dependency\":%u,\"dependency_id\":%" PRIu64 ",\"payload\":\"",
        out->prior_interval_certified ? "true" : "false", r->source, r->o, r->t, r->pid, r->g, r->q,
        (unsigned)r->rank, r->identity_known ? "true" : "false", r->fork_seed_proved ? "true" : "false",
        r->has_q ? "true" : "false", (unsigned)r->dependency, r->dependency_id) < 0) return false;
    const unsigned char *p = r->payload;
    for (size_t i = 0; i < r->payload_size; ++i) if (fprintf(f, "%02x", p[i]) < 0) return false;
    return fputs("\"}\n", f) >= 0 && fflush(f) == 0;
}

#if defined(__linux__) && !defined(PGWT_TRANSPORT_HOST_TEST)
/* Linux daemon binding; portable feed above remains free of kernel access. */
#include "daemon.h"
#include "capture_counters.h"
#include "event_stream.h"
#include <signal.h>
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
    return 0;
}
static int daemon_observe(void *ctx, pgwt_merge_observation *o)
{
    ma_snapshot s;
    int rc = pgwt_merge_capture_snapshot(ctx, &s, &o->d);
    if (rc < 0) return rc;
    o->counters.known = s.valid;
    for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i) {
        o->counters.value[i] = s.value[i];
        o->counters.known = o->counters.known && s.component_valid[i];
    }
    return 0;
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
    if (daemon_observe(d, &baseline) < 0 || !baseline.counters.known) return -EIO;
    struct pgwt_capture_config cfg = {.cpu_count = ncpu, .capacity = 262144,
        .baseline = baseline, .ctx = d, .lifecycle = daemon_lifecycle,
        .trace = daemon_trace, .observe = daemon_observe, .writer_failed = daemon_writer_failed,
        .sink = {.emit = daemon_sink, .ctx = d}};
    /* Create the dump first: creation can report a VOID through the sink. */
    size_t len = strlen(d->trace_dir);
    if (len > SIZE_MAX - 40) return -ENAMETOOLONG;
    char *path = malloc(len + 40);
    if (!path) return -ENOMEM;
    snprintf(path, len + 40, "%s/merge-qualification.jsonl", d->trace_dir);
    FILE *dump = fopen(path, "a"); free(path);
    if (!dump) return -errno;
    bool header_ok = fputs("{\"format\":\"qualification evidence only; not a product capture format\","
        "\"version\":1,\"mode\":\"N\",\"counter_order\":\"merge-v1 section 4\",\"cpus\":[", dump) >= 0;
    for (size_t i = 0; i < ncpu && header_ok; ++i)
        header_ok = fprintf(dump, "%s%u", i ? "," : "", pgwt_percpu_transport_cpu_id(d, i)) >= 0;
    if (!header_ok || fputs("]}\n", dump) < 0 || fflush(dump)) {
        fclose(dump); return -EIO;
    }
    /* Validated baseline and fixed capacities make ordinary construction
     * silent; a creation failure is reported out of band, never half-wired. */
    d->merge_capture = pgwt_capture_create(&cfg);
    if (!d->merge_capture) {
        fprintf(stderr, "exact-perf: production merge creation failed; capture stopped\n");
        fclose(dump); return -ENOMEM;
    }
    d->merge_capture->dump = dump;
    d->attach_report = daemon_attach; d->attach_report_ctx = d;
    return 0;
}
int pgwt_merge_capture_poll(struct pgwt_daemon *d)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    ma_snapshot s; uint64_t now;
    if (!d->merge_capture) return -EINVAL;
    int rc = pgwt_merge_capture_snapshot(d, &s, &now);
    if (rc < 0) { memset(&s, 0, sizeof(s)); now = 0; }
    ma_envelope e = {.version = 1, .kind = MA_CHECK};
    return pgwt_capture_envelope(d->merge_capture, &e, now, &s);
}
int pgwt_merge_capture_tick(struct pgwt_daemon *d)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    if (d->daemon_mode && kill(d->postmaster_pid, 0) != 0) {
        fprintf(stderr, "\npg_wait_tracer: PostgreSQL (PID %d) stopped\n", d->postmaster_pid);
        d->exit_reason = PGWT_EXIT_PG_DEAD; d->running = false; return 0;
    }
    pgwt_sweep_bootstrap_backends(d);
    if (!d->lightweight_mode && !getenv("PGWT_TEST_NO_RECOVERY")) pgwt_recover_unattached_backends(d);
    if (!d->lightweight_mode && !getenv("PGWT_TEST_NO_STALE_SWEEP")) pgwt_sweep_stale_state(d);
    pgwt_escalation_check_budget(d);
    return pgwt_merge_capture_poll(d);
}
int pgwt_merge_capture_finish(struct pgwt_daemon *d)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    if (!d->merge_capture || !d->percpu_transport) return -EINVAL;
    int rc = pgwt_transport_finish(d->percpu_transport);
    ma_snapshot s = {0}; uint64_t now = 0;
    pgwt_merge_capture_snapshot(d, &s, &now);
    int end = pgwt_capture_finish(d->merge_capture, now, &s);
    uint64_t age = pgwt_capture_max_held_ns(d->merge_capture, now), lost = 0;
    bool lost_known = pgwt_transport_lost_total(d->percpu_transport, &lost);
    pgwt_merge_report report = pgwt_capture_report(d->merge_capture);
    FILE *dump = d->merge_capture->dump;
    int saved = fprintf(dump, "{\"kind\":\"QUALIFICATION_END\",\"max_held_ns\":%llu,"
        "\"perf_lost_known\":%s,\"perf_lost_total\":%llu,\"status\":%u,\"epoch_invalid\":%s}\n",
        (unsigned long long)age, lost_known ? "true" : "false", (unsigned long long)lost,
        (unsigned)report.status, report.epoch_invalid ? "true" : "false");
    if (saved < 0 || fflush(dump)) {
        fprintf(stderr, "exact-perf: qualification summary could not be written\n");
        ++d->counters.writer_push_fail_total; end = -EIO;
    }
    fprintf(stderr, "exact-perf: max held age ns=%llu; status=%u; perf LOST=%llu%s\n",
        (unsigned long long)age, (unsigned)report.status, (unsigned long long)lost,
        lost_known ? "" : " (unknown overflow)");
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
    ma_snapshot s = {0}; uint64_t now = 0;
    pgwt_merge_capture_snapshot(d, &s, &now);
    return pgwt_capture_attach(d->merge_capture, r, now, &s);
}
void pgwt_merge_capture_close(struct pgwt_daemon *d)
{
    if (!d || !d->merge_capture) return;
    FILE *dump = d->merge_capture->dump;
    d->attach_report = NULL; d->attach_report_ctx = NULL;
    pgwt_capture_destroy(d->merge_capture); d->merge_capture = NULL;
    if (dump && fclose(dump)) fprintf(stderr, "exact-perf: qualification sink close failed\n");
}

#else
#include "daemon.h"
int pgwt_merge_capture_snapshot(struct pgwt_daemon *d, ma_snapshot *s, uint64_t *now)
{ (void)d; (void)s; (void)now; return -ENOTSUP; }
int pgwt_merge_capture_open(struct pgwt_daemon *d) { return !d ? -EINVAL : d->exact_perf ? -ENOTSUP : 0; }
int pgwt_merge_capture_poll(struct pgwt_daemon *d) { return !d ? -EINVAL : d->exact_perf ? -ENOTSUP : 0; }
int pgwt_merge_capture_tick(struct pgwt_daemon *d) { return !d ? -EINVAL : d->exact_perf ? -ENOTSUP : 0; }
int pgwt_merge_capture_finish(struct pgwt_daemon *d) { return !d ? -EINVAL : d->exact_perf ? -ENOTSUP : 0; }
int pgwt_merge_capture_record(struct pgwt_daemon *d, size_t s, const void *p, size_t n)
{ (void)s; (void)p; (void)n; return !d ? -EINVAL : d->exact_perf ? -ENOTSUP : 0; }
int pgwt_merge_capture_attach(struct pgwt_daemon *d, const struct pgwt_attach_report *r)
{ (void)r; return !d ? -EINVAL : d->exact_perf ? -ENOTSUP : 0; }
void pgwt_merge_capture_close(struct pgwt_daemon *d) { (void)d; }
#endif
