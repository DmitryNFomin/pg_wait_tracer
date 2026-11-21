#include "pgwt_merge_adapter.h"
#include <stdlib.h>
#include <string.h>

struct ma {
    pgwt_merge_mode mode;
    size_t count;
    ma_source_decl *declarations;
    pgwt_merge_source *sources;
};

static ma *create(const ma_config *c, bool production, pgwt_merge_reason *error)
{
    pgwt_merge_reason reason = PGWT_MERGE_MALFORMED_ENVELOPE;
    if (!c || !c->sources || c->source_count < 2 ||
        (unsigned)c->mode > PGWT_MERGE_Q ||
        c->source_count > SIZE_MAX / sizeof(ma_source_decl) ||
        c->source_count > SIZE_MAX / sizeof(pgwt_merge_source)) goto fail;
    reason = PGWT_MERGE_UNKNOWN_EVIDENCE;
    if (c->synthetic_premises == production) goto fail;
    for (size_t s = 0; s < c->source_count; ++s) {
        ma_mechanism expected = s < c->source_count - 2 ?
            MA_WATCHPOINT_PERF : MA_ORDERED_RING;
        if (!c->sources[s].qualification_complete ||
            c->sources[s].mechanism != expected) goto fail;
    }
    ma *a = calloc(1, sizeof(*a));
    if (a) {
        a->mode = c->mode;
        a->count = c->source_count;
        a->declarations = malloc(a->count * sizeof(*a->declarations));
        a->sources = malloc(a->count * sizeof(*a->sources));
    }
    if (!a || !a->declarations || !a->sources) {
        ma_destroy(a);
        reason = PGWT_MERGE_ALLOCATION_FAIL;
        goto fail;
    }
    memcpy(a->declarations, c->sources, a->count * sizeof(*a->declarations));
    for (size_t s = 0; s < a->count; ++s)
        a->sources[s] = (pgwt_merge_source){.qualified = true,
            .cutoff = a->declarations[s].initial_cutoff};
    return a;
fail:
    if (error) *error = reason;
    return NULL;
}

ma *ma_create(const ma_config *c, pgwt_merge_reason *error)
{
    return create(c, false, error);
}

ma *ma_create_production(const ma_config *c, pgwt_merge_reason *error)
{
    return create(c, true, error);
}

void ma_destroy(ma *a)
{
    if (!a) return;
    free(a->sources);
    free(a->declarations);
    free(a);
}

const pgwt_merge_source *ma_sources(const ma *a, size_t *count)
{
    if (count) *count = a ? a->count : 0;
    return a ? a->sources : NULL;
}

bool ma_core_config(const ma *a, const pgwt_merge_config *base, pgwt_merge_config *out)
{
    if (!a || !base || !out || base->sources) return false;
    *out = *base;
    out->mode = a->mode;
    out->sources = a->sources;
    out->source_count = a->count;
    return true;
}

static uint64_t le(const unsigned char *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

static ma_result faulted(ma_result r, pgwt_merge_reason reason)
{
    r.input[0].kind = PGWT_MERGE_FAULT;
    r.input[0].reason = reason;
    return r;
}

static uint64_t hash(uint64_t v)
{
    v = (v ^ (v >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    v = (v ^ (v >> 27)) * UINT64_C(0x94d049bb133111eb);
    return v ^ (v >> 31);
}
size_t ma_lifetime_bucket(const ma_table *t, uint64_t pid, uint64_t g, size_t *cost)
{
    if (cost) *cost = 0;
    if (!t->lifetime_index || !t->index_slots) return SIZE_MAX;
    size_t slot = (size_t)(hash(g ^ hash(pid)) % t->index_slots);
    for (;;) {
        if (cost) ++*cost;
        size_t entry = t->lifetime_index[slot];
        if (!entry || (t->lifetimes[entry - 1].pid == pid && t->lifetimes[entry - 1].g == g))
            return slot;
        slot = (slot + 1) % t->index_slots;
    }
}
size_t ma_binding_bucket(const ma_table *t, uint64_t query, size_t *cost)
{
    if (cost) *cost = 0;
    if (!t->binding_index || !t->index_slots) return SIZE_MAX;
    size_t slot = (size_t)(hash(query) % t->index_slots);
    for (;;) {
        if (cost) ++*cost;
        size_t entry = t->binding_index[slot];
        if (!entry || t->bindings[entry - 1].query_id == query) return slot;
        slot = (slot + 1) % t->index_slots;
    }
}
static const ma_lifetime *lifetime(const ma_table *t, uint64_t pid, uint64_t g)
{
    const ma_lifetime *found = NULL;
    if (!g) return NULL;
    size_t slot = ma_lifetime_bucket(t, pid, g, NULL);
    if (slot != SIZE_MAX) {
        size_t entry = t->lifetime_index[slot];
        if (entry) found = &t->lifetimes[entry - 1];
    } else for (size_t i = 0; i < t->lifetime_count; ++i) {
        const ma_lifetime *l = &t->lifetimes[i];
        if (l->pid != pid || l->g != g) continue;
        if (found) return NULL;          /* no unique historical binding */
        found = l;
    }
    return found && found->identity_bound ? found : NULL;
}

static bool proof_complete(const ma *a, const ma_lifetime *l)
{
    uint64_t required = MA_ALL_FACTS;
    if (a->mode == PGWT_MERGE_N) required &= ~(uint64_t)MA_SEQUENCE;
    return (l->facts_known & l->facts_true & required) == required;
}

/* This checks declarations, never TEXT availability (which is core-owned). */
static pgwt_merge_reason binding(const ma_table *t, uint64_t query,
                                 pgwt_merge_dependency *dep, uint64_t *id,
                                 bool *ok)
{
    *dep = PGWT_MERGE_READY;
    *id = 0;
    *ok = true;
    if (!query) return PGWT_MERGE_UNKNOWN_EVIDENCE;
    const ma_binding *b = NULL;
    size_t slot = ma_binding_bucket(t, query, NULL);
    if (slot != SIZE_MAX) {
        size_t entry = t->binding_index[slot];
        if (entry) b = &t->bindings[entry - 1];
    } else for (size_t i = 0; i < t->binding_count; ++i) {
        if (t->bindings[i].query_id != query) continue;
        if (b) { *ok = false; return PGWT_MERGE_DEPENDENCY_LOST; }
        b = &t->bindings[i];
    }
    if (!b) { *ok = false; return PGWT_MERGE_UNKNOWN_EVIDENCE; }
    if ((unsigned)b->kind > MA_LOST ||
        (b->kind == MA_NOT_REQUIRED ? b->dependency_id != 0 : b->dependency_id == 0)) {
        *ok = false;
        return PGWT_MERGE_DEPENDENCY_LOST;
    }
    if (b->dependency_id) for (size_t i = 0; i < t->binding_count; ++i)
        if (t->bindings[i].query_id != query &&
            t->bindings[i].dependency_id == b->dependency_id) {
            *ok = false;
            return PGWT_MERGE_DEPENDENCY_LOST;
        }
    *id = b->dependency_id;
    if (b->kind == MA_WAIT) *dep = PGWT_MERGE_WAIT;
    if (b->kind == MA_LOST) *dep = PGWT_MERGE_LOST;
    return PGWT_MERGE_UNKNOWN_EVIDENCE; /* ignored on success */
}

static ma_result attachment_event(const ma *a, const ma_envelope *e,
                                   const ma_table *t, ma_result r)
{
    uint64_t g = e->cookie_present && e->cookie ? e->cookie : 0;
    const ma_lifetime *l = lifetime(t, e->control_pid, g);
    if (!l) return faulted(r, PGWT_MERGE_UNKNOWN_EVIDENCE);
    if ((unsigned)e->attach_event > MA_NONIDEMPOTENT_INIT ||
        (unsigned)l->proof > MA_FAILED)
        return faulted(r, PGWT_MERGE_MALFORMED_ENVELOPE);
    ma_lifetime next = *l;
    bool failed = false;
    switch (e->attach_event) {
    case MA_BEGIN:
        if (l->proof == MA_FAILED) break; /* terminal; never launder */
        if (l->fork_forwarded || l->proof == MA_PROVED) failed = true;
        else next.proof = MA_PENDING;
        break;
    case MA_COMPLETE:
        if (l->proof == MA_FAILED) break;
        next.facts_known = e->facts_known;
        next.facts_true = e->facts_true;
        if (l->proof != MA_PENDING || !proof_complete(a, &next)) failed = true;
        else next.proof = MA_PROVED;
        break;
    case MA_INTERRUPTED:
    case MA_DESTRUCTIVE_RESEED:
    case MA_NONIDEMPOTENT_INIT:
        failed = true; /* every separately delivered coverage break faults */
        break;
    }
    if (failed) {
        next.proof = MA_FAILED;
        next.failure_reported = true;
        r = faulted(r, PGWT_MERGE_UNKNOWN_EVIDENCE);
    }
    r.has_lifetime_update = true;
    r.next_lifetime = next;
    return r;
}

pgwt_discovery_exit_class ma_discovery_exit(const pgwt_discovery_resolution *r)
{
    if (r->fork_cookie && r->fork_cookie < PGWT_DISCOVERY_COOKIE_MIN)
        return PGWT_DISC_EXIT_FORK;
    if (r->tid != r->tgid || r->parent != r->postmaster)
        return PGWT_DISC_EXIT_NONMEMBER;
    if (!r->start_known || !r->start || !r->clocks_qualified)
        return PGWT_DISC_EXIT_UNKNOWN;
    if (r->start >= r->F0) return PGWT_DISC_EXIT_MISSING_FORK;
    return PGWT_DISC_EXIT_PREEXISTING;
}

ma_result ma_map(const ma *a, const ma_envelope *e, const ma_table *t,
                 uint64_t d, const ma_snapshot *snapshot)
{
    ma_result r = {.count = 1, .input = {{.kind = PGWT_MERGE_CHECK,
        .observation = {.d = d}}}};
    pgwt_merge_counters *c = &r.input[0].observation.counters;
    c->known = snapshot && snapshot->valid;
    if (snapshot) for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i) {
        c->value[i] = snapshot->value[i];
        c->known = c->known && snapshot->component_valid[i];
    }
    if (!a || !e || !t || e->version != 1 ||
        (t->lifetime_count && !t->lifetimes) || (t->binding_count && !t->bindings) ||
        (t->held && t->source_count != a->count) ||
        (unsigned)e->kind > MA_DISCOVERY_CONTROL || (unsigned)e->origin > MA_ADMIN)
        return faulted(r, PGWT_MERGE_MALFORMED_ENVELOPE);
    if (e->kind == MA_DISCOVERY_CONTROL) {
        if (e->size || (unsigned)e->discovery.kind > PGWT_DISC_WAIT_LIMIT)
            return faulted(r, PGWT_MERGE_MALFORMED_ENVELOPE);
        r.input[0].kind = e->discovery.kind == PGWT_DISC_ADMIT ? PGWT_MERGE_ADMIT_DISCOVERY :
            e->discovery.kind == PGWT_DISC_CONFIRM ? PGWT_MERGE_CONFIRM_DISCOVERY :
            e->discovery.kind == PGWT_DISC_TERMINAL ? PGWT_MERGE_TERMINAL_DISCOVERY :
            e->discovery.kind == PGWT_DISC_ABANDON ? PGWT_MERGE_ABANDON_DISCOVERY :
            PGWT_MERGE_DISCOVERY_OBSERVATION;
        r.input[0].discovery = e->discovery;
        return r;
    }
    bool positioned = e->kind == MA_TRACE || e->kind == MA_LIFECYCLE ||
                      e->kind == MA_TEXT || e->kind == MA_BOUNDARY;
    if ((positioned || e->kind == MA_LOSS) && e->source >= a->count)
        return faulted(r, PGWT_MERGE_MALFORMED_ENVELOPE);
    if (positioned && t->held && t->held[e->source] && t->held[e->source] != e)
        return faulted(r, PGWT_MERGE_QUEUE_OWNERSHIP);
    if (e->kind == MA_CHECK || e->kind == MA_WRITER_REJECT ||
        e->kind == MA_ATTACH_EVENT || e->kind == MA_BOUNDARY || e->kind == MA_LOSS) {
        if (e->size) return faulted(r, PGWT_MERGE_MALFORMED_ENVELOPE);
        switch (e->kind) {
        case MA_CHECK: return r;
        case MA_WRITER_REJECT:
            r.input[0].kind = PGWT_MERGE_WRITER_REJECTED;
            return r;
        case MA_ATTACH_EVENT: return attachment_event(a, e, t, r);
        case MA_LOSS:
            if (a->declarations[e->source].mechanism != MA_WATCHPOINT_PERF)
                return faulted(r, PGWT_MERGE_MALFORMED_ENVELOPE);
            r.input[0].kind = PGWT_MERGE_LOSS;
            r.input[0].source = e->source;
            r.input[0].n = e->lost;
            return r;
        case MA_BOUNDARY:
            if (e->boundary == MA_NO_BOUNDARY) { r.hold = true; return r; }
            if (e->boundary != (a->declarations[e->source].mechanism ==
                MA_WATCHPOINT_PERF ? MA_IDLE_PERF : MA_RING_BOUNDARY))
                return faulted(r, PGWT_MERGE_MALFORMED_ENVELOPE);
            r.input[0].kind = PGWT_MERGE_BOUNDARY;
            r.input[0].source = e->source;
            r.input[0].p = e->position;
            r.input[0].w = e->w;
            return r;
        default: break;
        }
    }
    size_t size = e->kind == MA_TRACE ? 48 : e->kind == MA_TEXT ? 272 : 24;
    if (!e->bytes || e->size != size ||
        (e->kind != MA_TRACE && e->source != a->count - 1) ||
        (e->kind == MA_TRACE && e->source == a->count - 1))
        return faulted(r, PGWT_MERGE_MALFORMED_ENVELOPE);
    if (e->origin != MA_EXACT) return faulted(r, PGWT_MERGE_UNKNOWN_EVIDENCE);
    pgwt_merge_input *in = &r.input[0];
    pgwt_merge_record record = {.source = e->source, .o = e->position,
        .payload = e->bytes, .payload_size = e->size, .discovery = e->discovery_origin};
    if (e->kind == MA_TEXT) {
        if (le(e->bytes, 4) != 4) return faulted(r, PGWT_MERGE_MALFORMED_ENVELOPE);
        pgwt_merge_dependency dep;
        bool ok;
        pgwt_merge_reason reason = binding(t, le(e->bytes + 8, 8),
                                           &dep, &record.dependency_id, &ok);
        if (!ok) return faulted(r, reason);
        if (dep == PGWT_MERGE_LOST) return faulted(r, PGWT_MERGE_DEPENDENCY_LOST);
        if (record.discovery.exclusive_text) {
            record.pid = le(e->bytes + 4, 4);
            record.g = e->cookie_present ? e->cookie : 0;
            const ma_lifetime *owner = lifetime(t, record.pid, record.g);
            if (!owner || !owner->identity_bound || record.g < PGWT_DISCOVERY_COOKIE_MIN)
                return faulted(r, PGWT_MERGE_UNKNOWN_EVIDENCE);
            record.identity_known = true;
        }
        in->kind = PGWT_MERGE_TEXT;
        in->record = record;
        return r;
    }
    record.g = e->cookie_present && e->cookie ? e->cookie : 0;
    if (e->kind == MA_TRACE) {
        record.pid = le(e->bytes + 8, 4);
        record.t = le(e->bytes, 8);
        record.rank = PGWT_MERGE_TRANS;
        uint64_t old = le(e->bytes + 12, 4), flags = le(e->bytes + 20, 4);
        record.discovery.marker = record.discovery.marker || (old >= 0xfffffff0 && old <= 0xfffffff7);
        if (!record.pid || (flags & 1) || old == 0xfffffff4 || old == 0xfffffff5)
            return faulted(r, PGWT_MERGE_UNKNOWN_EVIDENCE);
        if (a->mode == PGWT_MERGE_Q) { record.q = e->q; record.has_q = e->has_q; }
        bool ok;
        pgwt_merge_reason reason = binding(t, le(e->bytes + 32, 8),
                                           &record.dependency, &record.dependency_id, &ok);
        if (!ok) return faulted(r, reason);
    } else {
        uint64_t tag = le(e->bytes, 4);
        if (tag < 1 || tag > 3) return faulted(r, PGWT_MERGE_MALFORMED_ENVELOPE);
        record.rank = tag == 1 ? PGWT_MERGE_FORK : tag == 2 ? PGWT_MERGE_INIT : PGWT_MERGE_EXIT;
        record.pid = le(e->bytes + 4, 4);
        record.t = le(e->bytes + 16, 8);
    }
    const ma_lifetime *l = lifetime(t, record.pid, record.g);
    record.identity_known = l != NULL;
    if (l && (unsigned)l->proof > MA_FAILED)
        return faulted(r, PGWT_MERGE_MALFORMED_ENVELOPE);
    if (record.rank == PGWT_MERGE_FORK && l) {
        if (l->proof == MA_PENDING) { r.hold = true; return r; }
        ma_lifetime next = *l;
        if (l->proof == MA_PROVED && !proof_complete(a, l)) next.proof = MA_FAILED;
        record.fork_seed_proved = next.proof == MA_PROVED;
        next.fork_forwarded = true;
        r.has_lifetime_update = true;
        if (next.proof == MA_FAILED && !next.failure_reported) {
            next.failure_reported = true;
            r = faulted(r, PGWT_MERGE_UNKNOWN_EVIDENCE);
            r.count = 2;
            r.input[1].observation = r.input[0].observation;
            in = &r.input[1];
        }
        r.next_lifetime = next;
    }
    in->kind = PGWT_MERGE_DATA;
    in->record = record;
    return r;
}
