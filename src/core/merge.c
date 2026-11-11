#include "pgwt_merge.h"

#include <stdlib.h>
#include <string.h>

struct source_state {
    pgwt_merge_source proof;
    bool has_position, has_data;
    uint64_t position, last_t, accepted_boundary;
};
struct generation_state {
    pgwt_merge_attachment proof;
    bool lifecycle_seen, sequence_seen, exited;
    unsigned creation;             /* 1: FORK proved, 2: INIT also proved */
    pgwt_merge_record exit_record;
};
struct frontier_state {
    uint64_t epoch, frontier;
    bool invalid;
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
    struct generation_state *attachments, *prospective;
    struct frontier_state *frontiers;
    uint64_t *dependencies;
    size_t count, attachment_count;
    size_t frontier_count, dependency_count;
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

static pgwt_merge_reason counter_fault(pgwt_merge_counters now,
                                       pgwt_merge_counters previous)
{
    if (!now.known) return PGWT_MERGE_COUNTER_UNKNOWN;
    if (previous.known)
        for (size_t i = 0; i < PGWT_MERGE_COUNTERS; ++i)
            if (now.value[i] < previous.value[i]) return PGWT_MERGE_COUNTER_UNKNOWN;
    return PGWT_MERGE_COUNTER_CHANGE;
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
    /* Discarded lifecycle evidence can never bootstrap a "new" generation
     * in a later epoch (DECISION 25a). Positions already remain consumed. */
    for (size_t i = 0; i < m->count; ++i) {
        const struct staged *v = &m->queue[i];
        if (v->text || v->record.rank == PGWT_MERGE_TRANS) continue;
        for (size_t j = 0; j < m->attachment_count; ++j)
            if (m->attachments[j].proof.pid == v->record.pid &&
                m->attachments[j].proof.g == v->record.g)
                m->attachments[j].lifecycle_seen = true;
    }
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
    for (size_t i = 0; i < m->frontier_count; ++i)
        if (in->record.t <= m->frontiers[i].frontier) {
            m->frontiers[i].invalid = true;
            m->report.epoch_invalid = true;
            m->report.last_invalid_epoch = m->frontiers[i].epoch;
        }
    return fault(m, reason, in->observation);
}

static struct generation_state *attachment(struct generation_state *a, size_t n,
                                            const pgwt_merge_record *r)
{
    for (size_t i = 0; i < n; ++i)
        if (a[i].proof.pid == r->pid && a[i].proof.g == r->g) return &a[i];
    return NULL;
}

static struct generation_state *remember_generation(pgwt_merge *m,
                                                     const pgwt_merge_record *r)
{
    struct generation_state *a = attachment(m->attachments, m->attachment_count, r);
    if (a) return a;
    if (m->attachment_count == m->config.attachment_capacity) return NULL;
    a = &m->attachments[m->attachment_count++];
    *a = (struct generation_state){.proof = {.pid = r->pid, .g = r->g}};
    return a;
}

static bool dependency_available(const pgwt_merge *m, uint64_t id)
{
    if (!id) return false;
    for (size_t i = 0; i < m->dependency_count; ++i)
        if (m->dependencies[i] == id) return true;
    return false;
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
        return fault(m, counter_fault(obs.counters, m->observed), obs);
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
        struct generation_state *generation = attachment(m->prospective, m->attachment_count, r);
        if (!r->identity_known || !generation)
            return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);
        pgwt_merge_attachment *a = &generation->proof;
        /* DECISION 25/25a: bootstrap only in validated output order. A FORK
         * cannot resurrect a known generation or overlap another live one. */
        if (!a->identity_proved) {
            if (r->rank != PGWT_MERGE_FORK || generation->lifecycle_seen ||
                generation->sequence_seen)
                return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);
            for (size_t j = 0; j < m->attachment_count; ++j) {
                const struct generation_state *other = &m->prospective[j];
                if (other == generation || other->proof.pid != r->pid) continue;
                /* A registry slot reserved by a later queued generation is
                 * not lifecycle/sequence evidence preceding this FORK. */
                if (!other->proof.identity_proved && !other->lifecycle_seen &&
                    !other->sequence_seen) continue;
                if (!other->exited || order(m, &other->exit_record, r) >= 0)
                    return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);
            }
            a->identity_proved = true;
            generation->creation = 1;
        } else if (generation->creation && r->rank == PGWT_MERGE_FORK) {
            return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);
        }
        if (generation->creation == 1 && r->rank == PGWT_MERGE_INIT) {
            generation->creation = 2;
            generation->sequence_seen = true;
            a->sequence_proved = true;
            a->next_q = 1;
        }
        if (generation->creation == 1 && r->rank != PGWT_MERGE_FORK)
            return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);
        if (r->rank != PGWT_MERGE_TRANS) generation->lifecycle_seen = true;
        if (r->rank == PGWT_MERGE_EXIT) {
            generation->exited = true;
            generation->exit_record = *r;
            generation->exit_record.payload = NULL;
        }
        if (r->dependency == PGWT_MERGE_LOST)
            return fault(m, PGWT_MERGE_DEPENDENCY_LOST, obs);
        if (r->dependency == PGWT_MERGE_WAIT &&
            !dependency_available(m, r->dependency_id)) held = true;
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
    bool new_frontier_epoch = !m->frontier_count ||
        m->frontiers[m->frontier_count - 1].epoch != m->report.epoch;
    if ((!m->report.has_frontier || W - 1 > m->report.frontier) &&
        new_frontier_epoch && m->frontier_count == m->config.frontier_capacity)
        return fault(m, PGWT_MERGE_ALLOCATION_FAIL, obs);
    for (size_t i = 0; i < eligible; ++i) {
        struct staged *v = &m->queue[i];
        pgwt_merge_output out = {.kind = v->text ? PGWT_MERGE_OUT_TEXT : PGWT_MERGE_OUT_DATA,
            .record = v->record, .epoch = m->report.epoch};
        /* contract §3/§4: rejection is not output; post carries failure C,d. */
        if (!m->config.sink(m->config.sink_context, &out, &obs))
            return fault(m, PGWT_MERGE_WRITER_PUSH_FAIL, obs);
        if (!obs.counters.known) return fault(m, PGWT_MERGE_COUNTER_UNKNOWN, obs);
        if (!same_counters(obs.counters, m->baseline))
            return fault(m, counter_fault(obs.counters, m->observed), obs);
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
        if (new_frontier_epoch) ++m->frontier_count;
        m->frontiers[m->frontier_count - 1] = (struct frontier_state){
            .epoch = m->report.epoch, .frontier = F};
        if (!obs.counters.known) return fault(m, PGWT_MERGE_COUNTER_UNKNOWN, obs);
        if (!same_counters(obs.counters, m->baseline))
            return fault(m, counter_fault(obs.counters, m->observed), obs);
    }
    return m->report.status;
}

/* contract §2/§4: source positions include boundaries and TEXT. A position
 * not greater than the consumed high-water mark is a duplicate/ownership breach. */
static pgwt_merge_status position(pgwt_merge *m, size_t s, uint64_t o,
                                  pgwt_merge_observation obs)
{
    if (s >= m->config.source_count)
        return fault(m, PGWT_MERGE_MALFORMED_ENVELOPE, obs);
    struct source_state *src = &m->sources[s];
    if (src->has_position && o <= src->position)
        return fault(m, PGWT_MERGE_DUPLICATE_IDENTITY, obs);
    src->has_position = true;
    src->position = o;
    return m->report.status;
}

static pgwt_merge_status check(pgwt_merge *m, pgwt_merge_observation obs)
{
    /* Observation checkpoint is independent of the valid epoch baseline. */
    if (!obs.counters.known) return fault(m, PGWT_MERGE_COUNTER_UNKNOWN, obs);
    if (!same_counters(obs.counters, m->observed))
        return fault(m, counter_fault(obs.counters, m->observed), obs);
    m->observed = obs.counters;
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
        if ((in->kind == PGWT_MERGE_DATA &&
             ((unsigned)r->rank > PGWT_MERGE_EXIT ||
              (unsigned)r->dependency > PGWT_MERGE_LOST)) ||
            (r->payload_size && !r->payload) ||
            (in->kind == PGWT_MERGE_TEXT && r->source != m->config.source_count - 1))
            return fault(m, PGWT_MERGE_MALFORMED_ENVELOPE, obs);
        struct source_state *src = &m->sources[r->source];
        bool perf = r->source < m->config.source_count - 2 && src->proof.qualified;
        if (in->kind == PGWT_MERGE_DATA) {
            if (perf && src->has_data && r->t < src->last_t)
                return late_fault(m, PGWT_MERGE_CLOCK_REGRESSION, in);
            if (r->t < src->accepted_boundary)
                return late_fault(m, PGWT_MERGE_SEAL_VIOLATION, in);
            if (!r->identity_known)
                return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);
            struct generation_state *a = remember_generation(m, r);
            if (!a) return fault(m, PGWT_MERGE_ALLOCATION_FAIL, obs);
            if (m->report.status != PGWT_MERGE_OK && r->rank != PGWT_MERGE_TRANS)
                a->lifecycle_seen = true;
            if (perf) {
                src->has_data = true;
                src->last_t = r->t;
                if (r->t > src->accepted_boundary) src->accepted_boundary = r->t;
            }
        }
        /* Suspension suppresses trusted output, never arrival validation. */
        if (m->report.status != PGWT_MERGE_OK) return m->report.status;
        if (m->count == m->config.staged_capacity ||
            r->payload_size > m->config.payload_capacity) {
            if (in->kind == PGWT_MERGE_DATA && r->rank != PGWT_MERGE_TRANS)
                attachment(m->attachments, m->attachment_count, r)->lifecycle_seen = true;
            return fault(m, PGWT_MERGE_ALLOCATION_FAIL, obs);
        }
        if (in->kind == PGWT_MERGE_TEXT && r->dependency_id &&
            !dependency_available(m, r->dependency_id)) {
            if (m->dependency_count == m->config.dependency_capacity)
                return fault(m, PGWT_MERGE_ALLOCATION_FAIL, obs);
            m->dependencies[m->dependency_count++] = r->dependency_id;
        }
        struct staged *v = &m->queue[m->count++];
        v->text = in->kind == PGWT_MERGE_TEXT;
        v->record = *r;
        if (m->config.mode == PGWT_MERGE_N || r->rank != PGWT_MERGE_TRANS)
            v->record.q = 0;
        if (r->payload_size) memcpy(v->bytes, r->payload, r->payload_size);
        v->record.payload = v->bytes;
        if (!v->text && perf) {
            /* contract §3: accepted qualified perf DATA supplies w=t. */
            if (r->t > src->proof.cutoff) src->proof.cutoff = r->t;
        }
        break;
    }
    case PGWT_MERGE_BOUNDARY:
        if (position(m, in->source, in->p, obs) == PGWT_MERGE_EXIT_REQUIRED)
            return m->report.status;
        /* contract §3: accepted boundaries accumulate max, never retract. */
        if (in->w > m->sources[in->source].accepted_boundary)
            m->sources[in->source].accepted_boundary = in->w;
        if (m->report.status != PGWT_MERGE_OK) return m->report.status;
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
        if (!same_counters(obs.counters, m->observed)) return check(m, obs);
        break;
    case PGWT_MERGE_REARM:
        /* DECISION 28/28a: active rejection preserves staging and proofs, but
         * the supplied observation still reports any counter fault. */
        if (m->report.status == PGWT_MERGE_OK) {
            if (check(m, obs) == PGWT_MERGE_EXIT_REQUIRED) return m->report.status;
            return PGWT_MERGE_REARM_REJECTED;
        }
        /* Explicit recovery resets release cutoffs only. Arrival promises,
         * DATA clocks, generation history and published frontiers survive. */
        if (!obs.counters.known) return fault(m, PGWT_MERGE_COUNTER_UNKNOWN, obs);
        if (in->attachment_count > SIZE_MAX / sizeof(*in->attachments))
            return fault(m, PGWT_MERGE_MALFORMED_ENVELOPE, obs);
        if (in->attachment_count > m->config.attachment_capacity)
            return fault(m, PGWT_MERGE_ALLOCATION_FAIL, obs);
        if (!in->attachment_proofs_fresh ||
            (m->identity_repair_needed && !in->identity_repaired) ||
            !proofs_valid(in->attachments, in->attachment_count))
            return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);
        if (m->config.mode == PGWT_MERGE_Q)
            for (size_t i = 0; i < in->attachment_count; ++i)
                if (!in->attachments[i].sequence_proved || !in->attachments[i].next_q)
                    return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);
        if (m->report.epoch == UINT64_MAX)
            return fault(m, PGWT_MERGE_MALFORMED_ENVELOPE, obs);
        size_t needed = 0;
        for (size_t i = 0; i < in->attachment_count; ++i) {
            pgwt_merge_record r = {.pid = in->attachments[i].pid, .g = in->attachments[i].g};
            if (!attachment(m->attachments, m->attachment_count, &r)) ++needed;
        }
        if (needed > m->config.attachment_capacity - m->attachment_count)
            return fault(m, PGWT_MERGE_ALLOCATION_FAIL, obs);
        ++m->report.epoch;
        m->count = 0;
        m->has_last = false;
        m->identity_repair_needed = false;
        m->report.status = PGWT_MERGE_OK;
        for (size_t s = 0; s < m->config.source_count; ++s)
            m->sources[s].proof.cutoff = 0;
        m->dependency_count = 0;
        for (size_t i = 0; i < m->attachment_count; ++i) {
            m->attachments[i].proof.identity_proved = false;
            m->attachments[i].proof.sequence_proved = false;
            m->attachments[i].creation = 0;
        }
        for (size_t i = 0; i < in->attachment_count; ++i) {
            pgwt_merge_record r = {.pid = in->attachments[i].pid, .g = in->attachments[i].g};
            struct generation_state *a = remember_generation(m, &r);
            a->proof = in->attachments[i];
            a->sequence_seen = true;
        }
        m->baseline = m->observed = obs.counters;
        break;
    default:
        return fault(m, PGWT_MERGE_MALFORMED_ENVELOPE, obs);
    }
    /* contract §3: one complete release evaluation per non-faulting input. */
    return release(m, obs);
}

/* Creation failures obey the same VOID/exit distinction as feed failures,
 * including failures detected before any coordinator memory exists. */
static pgwt_merge *create_fault(const pgwt_merge_config *c, pgwt_merge_status *status,
                                pgwt_merge_reason reason)
{
    if (status) *status = internal(reason) ? PGWT_MERGE_EXIT_REQUIRED : PGWT_MERGE_SUSPENDED;
    if (c && c->sink) {
        pgwt_merge_observation obs = c->observation;
        pgwt_merge_output out = {.kind = PGWT_MERGE_OUT_VOID, .d = obs.d,
            .epoch = 1, .counters = obs.counters, .reason = reason};
        (void)c->sink(c->sink_context, &out, &obs);
    }
    return NULL;
}

pgwt_merge *pgwt_merge_create(const pgwt_merge_config *c, pgwt_merge_status *status)
{
    if (!status || !c || !c->sink || !c->sources || c->source_count < 2 ||
        (unsigned)c->mode > PGWT_MERGE_Q || !c->staged_capacity)
        return create_fault(c, status, PGWT_MERGE_MALFORMED_ENVELOPE);
    /* contract §2/§4: exact size arithmetic; overflow is an internal breach. */
    if (c->source_count > SIZE_MAX / sizeof(struct source_state) ||
        c->staged_capacity > SIZE_MAX / sizeof(struct staged) ||
        c->attachment_capacity > SIZE_MAX / sizeof(struct generation_state) ||
        c->attachment_count > SIZE_MAX / sizeof(pgwt_merge_attachment) ||
        (c->frontier_capacity ? c->frontier_capacity : c->staged_capacity) >
            SIZE_MAX / sizeof(struct frontier_state) ||
        (c->dependency_capacity ? c->dependency_capacity : c->staged_capacity) >
            SIZE_MAX / sizeof(uint64_t) ||
        (c->payload_capacity && c->staged_capacity > SIZE_MAX / c->payload_capacity))
        return create_fault(c, status, PGWT_MERGE_MALFORMED_ENVELOPE);
    if (c->attachment_count > c->attachment_capacity)
        return create_fault(c, status, PGWT_MERGE_ALLOCATION_FAIL);
    if (!proofs_valid(c->attachments, c->attachment_count))
        return create_fault(c, status, PGWT_MERGE_UNKNOWN_EVIDENCE);
    if (c->mode == PGWT_MERGE_Q)
        for (size_t i = 0; i < c->attachment_count; ++i)
            if (!c->attachments[i].sequence_proved || !c->attachments[i].next_q)
                return create_fault(c, status, PGWT_MERGE_UNKNOWN_EVIDENCE);
    pgwt_merge *m = calloc(1, sizeof(*m));
    if (m) {
        m->config = *c;
        if (!m->config.frontier_capacity) m->config.frontier_capacity = c->staged_capacity;
        if (!m->config.dependency_capacity) m->config.dependency_capacity = c->staged_capacity;
        m->sources = calloc(c->source_count, sizeof(*m->sources));
        m->queue = calloc(c->staged_capacity, sizeof(*m->queue));
        m->attachments = calloc(c->attachment_capacity ? c->attachment_capacity : 1,
                                sizeof(*m->attachments));
        m->prospective = calloc(c->attachment_capacity ? c->attachment_capacity : 1,
                                sizeof(*m->prospective));
        m->frontiers = calloc(m->config.frontier_capacity, sizeof(*m->frontiers));
        m->dependencies = calloc(m->config.dependency_capacity, sizeof(*m->dependencies));
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
        !m->frontiers || !m->dependencies ||
        !m->queue[0].bytes) {
        pgwt_merge_destroy(m);
        return create_fault(c, status, PGWT_MERGE_ALLOCATION_FAIL);
    }
    m->report.status = c->suspended ? PGWT_MERGE_SUSPENDED : PGWT_MERGE_OK;
    m->report.epoch = 1;
    m->report.qualified = true;
    m->report.has_frontier = c->has_frontier;
    m->report.frontier = c->frontier;
    if (c->has_frontier) {
        m->frontier_count = 1;
        m->frontiers[0] = (struct frontier_state){.epoch = 1, .frontier = c->frontier};
    }
    for (size_t s = 0; s < c->source_count; ++s) {
        m->sources[s].proof = c->sources[s];
        m->sources[s].accepted_boundary = c->sources[s].cutoff;
        if (!c->sources[s].qualified) m->report.qualified = false;
    }
    m->attachment_count = c->attachment_count;
    for (size_t i = 0; i < c->attachment_count; ++i) {
        m->attachments[i].proof = c->attachments[i];
        m->attachments[i].sequence_seen = true;
    }
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
    free(m->frontiers);
    free(m->dependencies);
    free(m);
}

pgwt_merge_report pgwt_merge_get_report(const pgwt_merge *m)
{
    if (m) return m->report;
    return (pgwt_merge_report){.status = PGWT_MERGE_EXIT_REQUIRED};
}

pgwt_merge_epoch_report pgwt_merge_get_epoch_report(const pgwt_merge *m, uint64_t epoch)
{
    if (m) for (size_t i = 0; i < m->frontier_count; ++i)
        if (m->frontiers[i].epoch == epoch)
            return (pgwt_merge_epoch_report){.has_frontier = true,
                .frontier = m->frontiers[i].frontier, .invalid = m->frontiers[i].invalid};
    return (pgwt_merge_epoch_report){0};
}
