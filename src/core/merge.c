#include "pgwt_merge.h"

#include <stdlib.h>
#include <string.h>

struct source_state {
    pgwt_merge_source proof;
    bool has_position, has_data;
    uint64_t position, last_t;
};
struct staged {
    bool text;
    pgwt_merge_record record;
    unsigned char *bytes;
};
struct pgwt_merge {
    pgwt_merge_config config;
    pgwt_merge_report report;
    struct source_state *sources;
    struct staged *queue;
    unsigned char *payload_storage;
    pgwt_merge_attachment *attachments, *prospective;
    size_t count, attachment_count;
    pgwt_merge_counters baseline, observed;
    bool identity_repair_needed;
    bool has_last;
    pgwt_merge_record last;
};

const char *pgwt_merge_reason_name(pgwt_merge_reason r)
{
    static const char *const names[] = {
        "counter_change", "counter_unknown", "perf_lost", "clock_regression",
        "seal_violation", "ambiguous_tie", "sequence_gap", "unknown_evidence",
        "dependency_lost", "writer_push_fail", "allocation_fail",
        "duplicate_identity", "sort_order_breach", "sequence_reuse",
        "malformed_envelope", "queue_ownership"
    };
    return (unsigned)r < PGWT_MERGE_REASON_COUNT ? names[r] : NULL;
}

static bool same_counters(pgwt_merge_counters a, pgwt_merge_counters b)
{
    return a.known && b.known && memcmp(a.value, b.value, sizeof(a.value)) == 0;
}

static bool internal(pgwt_merge_reason r)
{
    return r >= PGWT_MERGE_DUPLICATE_IDENTITY;
}

/* contract §4: every fault attempts VOID, including during suspension. The
 * successful prefix is frozen; a failed VOID is reported out of band. */
static pgwt_merge_status fault(pgwt_merge *m, pgwt_merge_reason reason,
                               pgwt_merge_observation obs)
{
    bool breach = internal(reason);
    if (m->report.faults[reason] == UINT64_MAX) {
        reason = PGWT_MERGE_MALFORMED_ENVELOPE;
        breach = true;
    } else ++m->report.faults[reason];
    m->count = 0;
    m->report.status = breach ? PGWT_MERGE_EXIT_REQUIRED : PGWT_MERGE_SUSPENDED;
    if (reason == PGWT_MERGE_COUNTER_CHANGE || reason == PGWT_MERGE_COUNTER_UNKNOWN ||
        reason == PGWT_MERGE_PERF_LOST || reason == PGWT_MERGE_DEPENDENCY_LOST)
        m->identity_repair_needed = true;
    m->observed = obs.counters;
    pgwt_merge_output out = {.kind = PGWT_MERGE_OUT_VOID, .d = obs.d,
        .epoch = m->report.epoch, .counters = obs.counters, .reason = reason};
    if (!m->config.sink(m->config.sink_context, &out, &obs))
        m->report.void_failed = true;
    return m->report.status;
}

/* contract §4: late DATA disproving a certified prefix invalidates the epoch
 * in addition to its VOID, without fabricating an output record. */
static pgwt_merge_status late_fault(pgwt_merge *m, pgwt_merge_reason reason,
                                    const pgwt_merge_input *in)
{
    if (m->report.has_frontier && in->record.t <= m->report.frontier) {
        m->report.epoch_invalid = true;
        m->report.last_invalid_epoch = m->report.epoch;
    }
    return fault(m, reason, in->observation);
}

static pgwt_merge_attachment *attachment(pgwt_merge_attachment *a, size_t n,
                                         const pgwt_merge_record *r)
{
    for (size_t i = 0; i < n; ++i)
        if (a[i].pid == r->pid && a[i].g == r->g) return &a[i];
    return NULL;
}

/* contract §2: exact unsigned lexicographic key; N ties retain source order,
 * Q puts causal q before source. No subtraction-based comparisons. */
static int order(const pgwt_merge *m, const pgwt_merge_record *a,
                  const pgwt_merge_record *b)
{
#define CMP(field) do { if (a->field != b->field) return a->field < b->field ? -1 : 1; } while (0)
    CMP(t);
    CMP(pid);
    CMP(g);
    CMP(rank);
    if (m->config.mode == PGWT_MERGE_Q) { CMP(q); }
    CMP(source);
    CMP(o);
#undef CMP
    return 0;
}

static int staged_order(const pgwt_merge *m, const struct staged *a,
                         const struct staged *b)
{
    if (a->text != b->text) return a->text ? -1 : 1;
    if (a->text) return a->record.o < b->record.o ? -1 : a->record.o != b->record.o;
    return order(m, &a->record, &b->record);
}

static bool proofs_valid(const pgwt_merge_attachment *a, size_t n)
{
    if (n && !a) return false;
    for (size_t i = 0; i < n; ++i) {
        if (!a[i].identity_proved) return false;
        for (size_t j = 0; j < i; ++j)
            if (a[j].pid == a[i].pid && a[j].g == a[i].g) return false;
    }
    return true;
}

/* contract §3: validate the COMPLETE eligible batch before any TEXT/DATA
 * callbacks. A pending dependency holds the frontier too. Sequence state is
 * prospective until successful publication; first arrival never sets next_q. */
static pgwt_merge_status release(pgwt_merge *m, pgwt_merge_observation obs)
{
    if (m->report.status != PGWT_MERGE_OK) return m->report.status;
    uint64_t W = UINT64_MAX;
    for (size_t s = 0; s < m->config.source_count; ++s)
        if (m->sources[s].proof.cutoff < W) W = m->sources[s].proof.cutoff;
    /* contract §3 W=min; zero/unqualified cutoffs cannot certify release. */
    m->report.diagnostic_cutoff = W;
    if (W == 0 || !m->report.qualified) return m->report.status;
    if (!obs.counters.known) return fault(m, PGWT_MERGE_COUNTER_UNKNOWN, obs);
    if (!same_counters(obs.counters, m->baseline))
        return fault(m, PGWT_MERGE_COUNTER_CHANGE, obs);
    /* Fixed in-place insertion sort keeps payload slots paired with envelopes. */
    for (size_t i = 1; i < m->count; ++i) {
        struct staged v = m->queue[i];
        size_t j = i;
        while (j && staged_order(m, &v, &m->queue[j - 1]) < 0) {
            m->queue[j] = m->queue[j - 1];
            --j;
        }
        m->queue[j] = v;
    }
    size_t eligible = 0;
    while (eligible < m->count && (m->queue[eligible].text ||
           m->queue[eligible].record.t < W)) ++eligible; /* contract §3 strict t<W */
    memcpy(m->prospective, m->attachments,
           m->attachment_count * sizeof(*m->attachments));
    bool held = false;
    for (size_t i = 0; i < eligible; ++i) {
        const struct staged *v = &m->queue[i];
        if (v->text) continue;
        const pgwt_merge_record *r = &v->record;
        pgwt_merge_attachment *a = attachment(m->prospective, m->attachment_count, r);
        if (!r->identity_known || !a || !a->identity_proved)
            return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);
        if (r->dependency == PGWT_MERGE_LOST)
            return fault(m, PGWT_MERGE_DEPENDENCY_LOST, obs);
        if (r->dependency == PGWT_MERGE_WAIT) held = true;
        if (m->has_last && order(m, &m->last, r) >= 0)
            return fault(m, PGWT_MERGE_SORT_ORDER_BREACH, obs);
        if (r->rank != PGWT_MERGE_TRANS) continue;
        if (m->config.mode == PGWT_MERGE_N) {
            /* contract §2: any same-generation cross-source TRANS/MARKER tie. */
            for (size_t j = 0; j < i; ++j) {
                const pgwt_merge_record *b = &m->queue[j].record;
                if (!m->queue[j].text && b->rank == PGWT_MERGE_TRANS &&
                    r->t == b->t && r->pid == b->pid && r->g == b->g &&
                    r->source != b->source)
                    return fault(m, PGWT_MERGE_AMBIGUOUS_TIE, obs);
            }
        } else {
            /* contract §2: only a proved next_q may establish the baseline. */
            if (!r->has_q || !a->sequence_proved)
                return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);
            if (r->q < a->next_q) return fault(m, PGWT_MERGE_SEQUENCE_REUSE, obs);
            if (r->q > a->next_q) return fault(m, PGWT_MERGE_SEQUENCE_GAP, obs);
            if (a->next_q == UINT64_MAX)
                return fault(m, PGWT_MERGE_MALFORMED_ENVELOPE, obs);
            ++a->next_q;
        }
    }
    if (held) return m->report.status;
    for (size_t i = 0; i < eligible; ++i) {
        struct staged *v = &m->queue[i];
        pgwt_merge_output out = {.kind = v->text ? PGWT_MERGE_OUT_TEXT : PGWT_MERGE_OUT_DATA,
            .record = v->record, .epoch = m->report.epoch};
        /* contract §3/§4: rejection is not output; post carries failure C,d. */
        if (!m->config.sink(m->config.sink_context, &out, &obs))
            return fault(m, PGWT_MERGE_WRITER_PUSH_FAIL, obs);
        if (!obs.counters.known) return fault(m, PGWT_MERGE_COUNTER_UNKNOWN, obs);
        if (!same_counters(obs.counters, m->baseline))
            return fault(m, PGWT_MERGE_COUNTER_CHANGE, obs);
        if (!v->text) { m->last = v->record; m->has_last = true; }
    }
    memcpy(m->attachments, m->prospective,
           m->attachment_count * sizeof(*m->attachments));
    /* Recycle slots by rotation; payload ownership stays unique. */
    for (size_t i = 0; i < eligible; ++i) {
        struct staged done = m->queue[0];
        memmove(m->queue, m->queue + 1, (m->count - 1) * sizeof(*m->queue));
        m->queue[m->count - 1] = done;
        --m->count;
    }
    uint64_t F = W - 1; /* contract §3 F=W-1; W>0 was proved above */
    /* contract §3: empty eligible batches also advance, never repeat F. */
    if (!m->report.has_frontier || F > m->report.frontier) {
        pgwt_merge_output out = {.kind = PGWT_MERGE_OUT_FRONTIER, .F = F,
            .counters = m->baseline, .epoch = m->report.epoch};
        if (!m->config.sink(m->config.sink_context, &out, &obs))
            return fault(m, PGWT_MERGE_WRITER_PUSH_FAIL, obs);
        m->report.has_frontier = true;
        m->report.frontier = F;
        if (!obs.counters.known) return fault(m, PGWT_MERGE_COUNTER_UNKNOWN, obs);
        if (!same_counters(obs.counters, m->baseline))
            return fault(m, PGWT_MERGE_COUNTER_CHANGE, obs);
    }
    return m->report.status;
}

/* contract §2/§4: source positions include boundaries and TEXT. A position
 * not greater than the consumed high-water mark is a duplicate/ownership breach. */
static pgwt_merge_status position(pgwt_merge *m, size_t s, uint64_t o,
                                  pgwt_merge_observation obs)
{
    if (s >= m->config.source_count || !o)
        return fault(m, PGWT_MERGE_MALFORMED_ENVELOPE, obs);
    struct source_state *src = &m->sources[s];
    if (src->has_position && o <= src->position)
        return fault(m, PGWT_MERGE_DUPLICATE_IDENTITY, obs);
    src->has_position = true;
    src->position = o;
    return m->report.status;
}

pgwt_merge_status pgwt_merge_feed(pgwt_merge *m, const pgwt_merge_input *in)
{
    if (!m) return PGWT_MERGE_EXIT_REQUIRED;
    if (m->report.status == PGWT_MERGE_EXIT_REQUIRED) return m->report.status;
    if (!in) return fault(m, PGWT_MERGE_MALFORMED_ENVELOPE,
                          (pgwt_merge_observation){0});
    pgwt_merge_observation obs = in->observation;
    switch (in->kind) {
    case PGWT_MERGE_DATA:
    case PGWT_MERGE_TEXT: {
        const pgwt_merge_record *r = &in->record;
        /* contract §3: duplicate first, clock regression second, seal third. */
        if (position(m, r->source, r->o, obs) == PGWT_MERGE_EXIT_REQUIRED)
            return m->report.status;
        if ((unsigned)r->rank > PGWT_MERGE_EXIT ||
            (unsigned)r->dependency > PGWT_MERGE_LOST ||
            (r->payload_size && !r->payload) ||
            (in->kind == PGWT_MERGE_TEXT && r->source != m->config.source_count - 1))
            return fault(m, PGWT_MERGE_MALFORMED_ENVELOPE, obs);
        if (m->report.status != PGWT_MERGE_OK) return m->report.status;
        struct source_state *src = &m->sources[r->source];
        bool perf = r->source < m->config.source_count - 2 && src->proof.qualified;
        if (in->kind == PGWT_MERGE_DATA) {
            if (perf && src->has_data && r->t < src->last_t)
                return late_fault(m, PGWT_MERGE_CLOCK_REGRESSION, in);
            if (r->t < src->proof.cutoff)
                return late_fault(m, PGWT_MERGE_SEAL_VIOLATION, in);
            if (!r->identity_known)
                return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);
        }
        if (m->count == m->config.staged_capacity ||
            r->payload_size > m->config.payload_capacity)
            return fault(m, PGWT_MERGE_ALLOCATION_FAIL, obs);
        struct staged *v = &m->queue[m->count++];
        v->text = in->kind == PGWT_MERGE_TEXT;
        v->record = *r;
        if (m->config.mode == PGWT_MERGE_N || r->rank != PGWT_MERGE_TRANS)
            v->record.q = 0;
        if (r->payload_size) memcpy(v->bytes, r->payload, r->payload_size);
        v->record.payload = v->bytes;
        if (!v->text && perf) {
            src->has_data = true;
            src->last_t = r->t;
            /* contract §3: accepted qualified perf DATA supplies w=t. */
            if (r->t > src->proof.cutoff) src->proof.cutoff = r->t;
        }
        break;
    }
    case PGWT_MERGE_BOUNDARY:
        if (position(m, in->source, in->p, obs) == PGWT_MERGE_EXIT_REQUIRED)
            return m->report.status;
        if (m->report.status != PGWT_MERGE_OK) return m->report.status;
        /* contract §3: accepted boundaries accumulate max, never retract. */
        if (in->w > m->sources[in->source].proof.cutoff)
            m->sources[in->source].proof.cutoff = in->w;
        break;
    case PGWT_MERGE_LOSS:
        if (in->source >= m->config.source_count - 2)
            return fault(m, PGWT_MERGE_MALFORMED_ENVELOPE, obs);
        if (in->n) return fault(m, PGWT_MERGE_PERF_LOST, obs);
        break;
    case PGWT_MERGE_FAULT:
        if (!pgwt_merge_reason_name(in->reason))
            return fault(m, PGWT_MERGE_MALFORMED_ENVELOPE, obs);
        return fault(m, in->reason, obs);
    case PGWT_MERGE_WRITER_REJECTED:
        return fault(m, PGWT_MERGE_WRITER_PUSH_FAIL, obs);
    case PGWT_MERGE_CHECK:
        /* contract §4: observation checkpoint is independent of valid baseline. */
        if (!obs.counters.known) return fault(m, PGWT_MERGE_COUNTER_UNKNOWN, obs);
        if (!same_counters(obs.counters, m->observed))
            return fault(m, PGWT_MERGE_COUNTER_CHANGE, obs);
        m->observed = obs.counters;
        break;
    case PGWT_MERGE_REARM:
        /* contract §4: explicit recovery, fresh boundaries (all reset to zero),
         * fresh attachments, readable baseline, repaired lifecycle evidence. */
        if (!obs.counters.known) return fault(m, PGWT_MERGE_COUNTER_UNKNOWN, obs);
        if (!in->attachment_proofs_fresh ||
            (m->identity_repair_needed && !in->identity_repaired) ||
            !proofs_valid(in->attachments, in->attachment_count))
            return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);
        if (in->attachment_count > m->config.attachment_capacity)
            return fault(m, PGWT_MERGE_ALLOCATION_FAIL, obs);
        if (m->config.mode == PGWT_MERGE_Q)
            for (size_t i = 0; i < in->attachment_count; ++i)
                if (!in->attachments[i].sequence_proved)
                    return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);
        if (m->report.epoch == UINT64_MAX)
            return fault(m, PGWT_MERGE_MALFORMED_ENVELOPE, obs);
        ++m->report.epoch;
        m->count = 0;
        m->has_last = false;
        m->identity_repair_needed = false;
        m->report.status = PGWT_MERGE_OK;
        m->report.has_frontier = false;
        m->report.epoch_invalid = false;
        for (size_t s = 0; s < m->config.source_count; ++s) {
            m->sources[s].proof.cutoff = 0;
            m->sources[s].has_data = false;
        }
        m->attachment_count = in->attachment_count;
        if (in->attachment_count)
            memcpy(m->attachments, in->attachments,
                   in->attachment_count * sizeof(*m->attachments));
        m->baseline = m->observed = obs.counters;
        break;
    default:
        return fault(m, PGWT_MERGE_MALFORMED_ENVELOPE, obs);
    }
    /* contract §3: one complete release evaluation per non-faulting input. */
    return release(m, obs);
}

pgwt_merge *pgwt_merge_create(const pgwt_merge_config *c, pgwt_merge_status *status)
{
    *status = PGWT_MERGE_EXIT_REQUIRED;
    if (!c || !c->sink || !c->sources || c->source_count < 2 ||
        (unsigned)c->mode > PGWT_MERGE_Q || !c->staged_capacity ||
        c->attachment_count > c->attachment_capacity ||
        !proofs_valid(c->attachments, c->attachment_count)) return NULL;
    /* contract §2/§4: exact size arithmetic; overflow is an internal breach. */
    if (c->source_count > SIZE_MAX / sizeof(struct source_state) ||
        c->staged_capacity > SIZE_MAX / sizeof(struct staged) ||
        c->attachment_capacity > SIZE_MAX / sizeof(pgwt_merge_attachment) ||
        (c->payload_capacity && c->staged_capacity > SIZE_MAX / c->payload_capacity))
        return NULL;
    pgwt_merge *m = calloc(1, sizeof(*m));
    if (m) {
        m->config = *c;
        m->sources = calloc(c->source_count, sizeof(*m->sources));
        m->queue = calloc(c->staged_capacity, sizeof(*m->queue));
        m->attachments = calloc(c->attachment_capacity ? c->attachment_capacity : 1,
                                sizeof(*m->attachments));
        m->prospective = calloc(c->attachment_capacity ? c->attachment_capacity : 1,
                                sizeof(*m->prospective));
        if (m->queue) {
            /* contract §4: reserve every payload slot at creation. */
            unsigned char *bytes = calloc(c->staged_capacity,
                                           c->payload_capacity ? c->payload_capacity : 1);
            m->payload_storage = bytes;
            if (bytes) for (size_t i = 0; i < c->staged_capacity; ++i)
                m->queue[i].bytes = bytes + i * (c->payload_capacity ? c->payload_capacity : 1);
        }
    }
    if (!m || !m->sources || !m->queue || !m->attachments || !m->prospective ||
        !m->queue[0].bytes) {
        pgwt_merge_destroy(m);
        pgwt_merge_observation obs = c->observation;
        pgwt_merge_output out = {.kind = PGWT_MERGE_OUT_VOID, .d = obs.d,
            .epoch = 1, .counters = obs.counters, .reason = PGWT_MERGE_ALLOCATION_FAIL};
        (void)c->sink(c->sink_context, &out, &obs);
        *status = PGWT_MERGE_SUSPENDED;
        return NULL;
    }
    m->config = *c;
    m->report.status = c->suspended ? PGWT_MERGE_SUSPENDED : PGWT_MERGE_OK;
    m->report.epoch = 1;
    m->report.qualified = true;
    m->report.has_frontier = c->has_frontier;
    m->report.frontier = c->frontier;
    for (size_t s = 0; s < c->source_count; ++s) {
        m->sources[s].proof = c->sources[s];
        if (!c->sources[s].qualified) m->report.qualified = false;
    }
    m->attachment_count = c->attachment_count;
    if (c->attachment_count)
        memcpy(m->attachments, c->attachments, c->attachment_count * sizeof(*m->attachments));
    m->baseline = c->observation.counters;
    m->observed = c->last_observed;
    if (!m->baseline.known) (void)fault(m, PGWT_MERGE_COUNTER_UNKNOWN, c->observation);
    *status = m->report.status;
    return m;
}

void pgwt_merge_destroy(pgwt_merge *m)
{
    if (!m) return;
    free(m->payload_storage);
    free(m->queue);
    free(m->sources);
    free(m->attachments);
    free(m->prospective);
    free(m);
}

pgwt_merge_report pgwt_merge_get_report(const pgwt_merge *m)
{
    if (m) return m->report;
    return (pgwt_merge_report){.status = PGWT_MERGE_EXIT_REQUIRED};
}
