#include "pgwt_merge.h"

#include <stdlib.h>
#include <string.h>

#ifdef PGWT_MERGE_TEST_WORK
#include "core_merge_work.h"
#define MERGE_WORK(kind, count) pgwt_merge_test_work(PGWT_WORK_##kind, (count))
#else
#define MERGE_WORK(kind, count) ((void)0)
#endif

struct source_state {
    pgwt_merge_source proof;
    bool has_position, has_data;
    uint64_t position, last_t, accepted_boundary;
};
struct generation_state {
    pgwt_merge_attachment proof;
    bool lifecycle_seen, sequence_seen, exited;
    bool fork_seed_admitted, first_released;
    unsigned creation;             /* 1: FORK awaiting INIT, 2: creation admitted */
    pgwt_merge_record exit_record;
    size_t next_pid;                /* one-based link in the PID hash bucket */
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
#ifdef PGWT_MERGE_TEST_WORK
/* Count literal memory moves too: a regression must not supply its own work
 * hook to be caught. Round bytes up to staged-envelope equivalents and call
 * the hook before the move can damage indexed slots. Production has neither
 * the wrapper nor the macro. New traversals still need node-visit hooks. */
static inline __attribute__((unused)) void *
queue_work_memmove(void *to, const void *from, size_t bytes)
{
    MERGE_WORK(QMOVE, bytes / sizeof(struct staged) + (bytes % sizeof(struct staged) != 0));
    return memmove(to, from, bytes);
}
#undef memmove
#define memmove(to, from, bytes) queue_work_memmove((to), (from), (bytes))
#endif
struct queue_node {
    size_t left, right, next;
    unsigned height;
};
struct dependency_entry {
    uint64_t id, epoch;
};
struct waiting_entry {
    uint64_t id;
    size_t count;
    bool used;
};
struct pgwt_merge {
    pgwt_merge_config config;
    pgwt_merge_report report;
    struct source_state *sources;
    struct staged *queue;
    struct queue_node *queue_nodes;
    unsigned char *payload_storage;
    struct generation_state *attachments, *prospective;
    size_t *generation_index, *pid_heads, *touched, *planned;
    size_t index_capacity, touched_count;
    struct frontier_state *frontiers;
    uint64_t *dependencies;
    size_t count, attachment_count;
    size_t root, first, free_slot;
    size_t frontier_count, dependency_count;
    pgwt_merge_counters baseline, observed;
    bool identity_repair_needed;
    bool held_cached;
    size_t held_tail, held_count, held_transition;
    struct dependency_entry *dependency_index;
    struct waiting_entry *waiting_index;
    size_t *waiting_slots;
    size_t dependency_index_capacity, waiting_index_capacity;
    size_t waiting_slot_count, waiting_count;
    bool has_last;
    pgwt_merge_record last;
};

static void reset_waiting(pgwt_merge *m);

/* Keep accounting tied to the size of the copy, including test mutants that
 * replace a touched-slot copy with a retained-table copy. */
static void copy_generations(struct generation_state *to,
                              const struct generation_state *from, size_t count)
{
    MERGE_WORK(COPY, count);
    memcpy(to, from, count * sizeof(*to));
}

static uint64_t mix(uint64_t x)
{
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

/* At most half full, no deletions: generations and their index entries live
 * for the whole capture, including across failed releases and REARM. */
static size_t generation_bucket(const pgwt_merge *m, const pgwt_merge_record *r)
{
    MERGE_WORK(LOOKUP, 0);
    MERGE_WORK(INDEX, 1);
    size_t slot = (size_t)mix(mix(r->pid) ^ r->g) & (m->index_capacity - 1);
    while (m->generation_index[slot]) {
        MERGE_WORK(STATE, 1);
        const pgwt_merge_attachment *a =
            &m->attachments[m->generation_index[slot] - 1].proof;
        if (a->pid == r->pid && a->g == r->g) break;
        slot = (slot + 1) & (m->index_capacity - 1);
        MERGE_WORK(INDEX, 1);
    }
    return slot;
}

static struct generation_state *attachment(pgwt_merge *m, const pgwt_merge_record *r)
{
    size_t entry = m->generation_index[generation_bucket(m, r)];
    MERGE_WORK(STATE, entry ? 1 : 0);
    return entry ? &m->attachments[entry - 1] : NULL;
}

/* touched[plan slot] names the retained slot; planned[retained slot] is the
 * one-based reverse map. Only eligible DATA touches prospective state. A held
 * prefix keeps its plan while newly eligible suffixes validate. Once the last
 * dependency resolves, publication revalidates the complete batch from the
 * published state. Fault recovery also starts from published state. */
static void reset_plan(pgwt_merge *m)
{
    for (size_t i = 0; i < m->touched_count; ++i) {
        MERGE_WORK(STATE, 1);
        m->planned[m->touched[i]] = 0;
    }
    m->touched_count = 0;
}

static struct generation_state *plan_generation(pgwt_merge *m,
                                                const pgwt_merge_record *r)
{
    struct generation_state *a = attachment(m, r);
    if (!a) return NULL;
    size_t slot = (size_t)(a - m->attachments);
    MERGE_WORK(STATE, 1);
    if (!m->planned[slot]) {
        copy_generations(&m->prospective[m->touched_count], a, 1);
        m->touched[m->touched_count++] = slot;
        m->planned[slot] = m->touched_count;
    }
    return &m->prospective[m->planned[slot] - 1];
}

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
    for (size_t i = m->first; i != SIZE_MAX;) {
        MERGE_WORK(QVISIT, 1);
        struct staged *v = &m->queue[i];
        if (!v->text && v->record.rank != PGWT_MERGE_TRANS) {
            struct generation_state *a = attachment(m, &v->record);
            if (a) a->lifecycle_seen = true;
        }
        size_t next = m->queue_nodes[i].next;
        m->queue_nodes[i].next = m->free_slot;
        m->free_slot = i;
        i = next;
    }
    m->root = m->first = SIZE_MAX;
    m->held_cached = false;
    reset_waiting(m);
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

static struct generation_state *remember_generation(pgwt_merge *m,
                                                     const pgwt_merge_record *r)
{
    size_t bucket = generation_bucket(m, r);
    size_t entry = m->generation_index[bucket];
    MERGE_WORK(STATE, 1);
    if (entry) return &m->attachments[entry - 1];
    if (m->attachment_count == m->config.attachment_capacity) return NULL;
    struct generation_state *a = &m->attachments[m->attachment_count++];
    size_t pid_bucket = (size_t)mix(r->pid) & (m->index_capacity - 1);
    *a = (struct generation_state){.proof = {.pid = r->pid, .g = r->g},
        .next_pid = m->pid_heads[pid_bucket]};
    m->pid_heads[pid_bucket] = m->attachment_count;
    m->generation_index[bucket] = m->attachment_count;
    return a;
}

/* Resolved metadata lives for the whole epoch, just as dependencies[] does.
 * Epoch tags clear its index on REARM without visiting unused capacity. */
static size_t dependency_bucket(const pgwt_merge *m, uint64_t id)
{
    size_t slot = (size_t)mix(id) & (m->dependency_index_capacity - 1);
    while (m->dependency_index[slot].epoch == m->report.epoch) {
        MERGE_WORK(QCOMPARE, 1);
        if (m->dependency_index[slot].id == id) return slot;
        slot = (slot + 1) & (m->dependency_index_capacity - 1);
    }
    return slot;
}

static bool dependency_available(const pgwt_merge *m, uint64_t id)
{
    return id && m->dependency_index[dependency_bucket(m, id)].epoch == m->report.epoch;
}

static size_t waiting_bucket(const pgwt_merge *m, uint64_t id)
{
    size_t slot = (size_t)mix(id) & (m->waiting_index_capacity - 1);
    while (m->waiting_index[slot].used) {
        MERGE_WORK(QCOMPARE, 1);
        if (m->waiting_index[slot].id == id) return slot;
        slot = (slot + 1) & (m->waiting_index_capacity - 1);
    }
    return slot;
}

static void wait_for_dependency(pgwt_merge *m, uint64_t id)
{
    size_t slot = waiting_bucket(m, id);
    struct waiting_entry *entry = &m->waiting_index[slot];
    if (!entry->used) {
        *entry = (struct waiting_entry){.id = id, .used = true};
        m->waiting_slots[m->waiting_slot_count++] = slot;
    }
    ++entry->count;
    ++m->waiting_count;
}

static void remember_dependency(pgwt_merge *m, uint64_t id)
{
    m->dependency_index[dependency_bucket(m, id)] =
        (struct dependency_entry){.id = id, .epoch = m->report.epoch};
    struct waiting_entry *entry = &m->waiting_index[waiting_bucket(m, id)];
    if (id && entry->used) {
        m->waiting_count -= entry->count;
        entry->count = 0;
    }
}

static void reset_waiting(pgwt_merge *m)
{
    for (size_t i = 0; i < m->waiting_slot_count; ++i) {
        MERGE_WORK(QVISIT, 1);
        m->waiting_index[m->waiting_slots[i]].used = false;
    }
    m->waiting_slot_count = m->waiting_count = 0;
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
    MERGE_WORK(QCOMPARE, 1);
    if (a->text != b->text) return a->text ? -1 : 1;
    if (a->text) return a->record.o < b->record.o ? -1 : a->record.o != b->record.o;
    return order(m, &a->record, &b->record);
}

/* Fixed payload slots form an AVL index and an in-order singly linked list.
 * Insertion and minimum removal take O(log n), with no envelope/payload moves;
 * visiting an eligible prefix takes O(k). Equal keys insert after existing
 * equals, preserving the former stable insertion sort, including TEXT ties. */
static unsigned queue_height(const pgwt_merge *m, size_t i)
{
    return i == SIZE_MAX ? 0 : m->queue_nodes[i].height;
}

static void queue_height_update(pgwt_merge *m, size_t i)
{
    unsigned l = queue_height(m, m->queue_nodes[i].left);
    unsigned r = queue_height(m, m->queue_nodes[i].right);
    m->queue_nodes[i].height = 1 + (l > r ? l : r);
}

static size_t queue_rotate(pgwt_merge *m, size_t i, bool left)
{
    size_t j = left ? m->queue_nodes[i].right : m->queue_nodes[i].left;
    if (left) {
        m->queue_nodes[i].right = m->queue_nodes[j].left;
        m->queue_nodes[j].left = i;
    } else {
        m->queue_nodes[i].left = m->queue_nodes[j].right;
        m->queue_nodes[j].right = i;
    }
    queue_height_update(m, i);
    queue_height_update(m, j);
    return j;
}

static size_t queue_balance(pgwt_merge *m, size_t i)
{
    queue_height_update(m, i);
    int balance = (int)queue_height(m, m->queue_nodes[i].left) -
                  (int)queue_height(m, m->queue_nodes[i].right);
    if (balance > 1) {
        size_t l = m->queue_nodes[i].left;
        if (queue_height(m, m->queue_nodes[l].left) < queue_height(m, m->queue_nodes[l].right))
            m->queue_nodes[i].left = queue_rotate(m, l, true);
        return queue_rotate(m, i, false);
    }
    if (balance < -1) {
        size_t r = m->queue_nodes[i].right;
        if (queue_height(m, m->queue_nodes[r].right) < queue_height(m, m->queue_nodes[r].left))
            m->queue_nodes[i].right = queue_rotate(m, r, false);
        return queue_rotate(m, i, true);
    }
    return i;
}

static size_t queue_insert(pgwt_merge *m, size_t root, size_t slot,
                            size_t previous, size_t next)
{
    if (root == SIZE_MAX) {
        m->queue_nodes[slot].left = m->queue_nodes[slot].right = SIZE_MAX;
        m->queue_nodes[slot].height = 1;
        m->queue_nodes[slot].next = next;
        if (previous == SIZE_MAX) m->first = slot;
        else m->queue_nodes[previous].next = slot;
        return slot;
    }
    if (staged_order(m, &m->queue[slot], &m->queue[root]) < 0)
        m->queue_nodes[root].left = queue_insert(m, m->queue_nodes[root].left, slot, previous, root);
    else
        m->queue_nodes[root].right = queue_insert(m, m->queue_nodes[root].right, slot, root, next);
    return queue_balance(m, root);
}

static size_t queue_remove_min(pgwt_merge *m, size_t root)
{
    MERGE_WORK(QVISIT, 1);
    if (m->queue_nodes[root].left == SIZE_MAX) return m->queue_nodes[root].right;
    m->queue_nodes[root].left = queue_remove_min(m, m->queue_nodes[root].left);
    return queue_balance(m, root);
}

static void queue_recycle_min(pgwt_merge *m)
{
    size_t slot = m->first;
    m->root = queue_remove_min(m, m->root);
    m->first = m->queue_nodes[slot].next;
    m->queue_nodes[slot].next = m->free_slot;
    m->free_slot = slot;
    --m->count;
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
    /* Fixed payload slots already follow the exact stable output order.
     * Valid DATA has t >= its source cutoff >= the previous W, so it cannot
     * enter a previously validated prefix. TEXT changes no generation plan.
     * Resume at the tail's CURRENT successor: later DATA may sort before the
     * record that used to be the first ineligible one. */
    size_t begin = m->first, prior_count = 0, tail = SIZE_MAX;
    size_t previous_slot = SIZE_MAX;
    if (m->held_cached && m->waiting_count) {
        begin = m->queue_nodes[m->held_tail].next;
        prior_count = m->held_count;
        tail = m->held_tail;
        previous_slot = m->held_transition;
        MERGE_WORK(QVISIT, begin == SIZE_MAX ? 0 : 1);
        MERGE_WORK(QCOMPARE, begin == SIZE_MAX ? 0 : 1);
        if (begin == SIZE_MAX || m->queue[begin].record.t >= W)
            return m->report.status;
    } else {
        reset_plan(m);
        reset_waiting(m);
    }
    m->held_cached = false;
    size_t eligible = begin, eligible_count = prior_count;
    while (eligible != SIZE_MAX && (m->queue[eligible].text ||
           m->queue[eligible].record.t < W)) {
        MERGE_WORK(QVISIT, 1);
        MERGE_WORK(QCOMPARE, 1);
        ++eligible_count;
        tail = eligible;
        eligible = m->queue_nodes[eligible].next;
    } /* contract §3 strict t<W */
    if (eligible != SIZE_MAX) {
        MERGE_WORK(QVISIT, 1);
        MERGE_WORK(QCOMPARE, 1);
    }
    size_t end = eligible;
    eligible = eligible_count;
    bool held = m->waiting_count != 0;
    const pgwt_merge_record *previous_transition = previous_slot == SIZE_MAX ?
        NULL : &m->queue[previous_slot].record;
    for (size_t i = begin; i != end; i = m->queue_nodes[i].next) {
        MERGE_WORK(QVISIT, 1);
        const struct staged *v = &m->queue[i];
        if (v->text) continue;
        const pgwt_merge_record *r = &v->record;
        struct generation_state *generation = plan_generation(m, r);
        if (!r->identity_known || !generation)
            return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);
        pgwt_merge_attachment *a = &generation->proof;
        /* DECISION 25/25a: bootstrap only in validated output order. A FORK
         * cannot resurrect a known generation or overlap another live one. */
        if (!a->identity_proved) {
            if (r->rank != PGWT_MERGE_FORK || generation->lifecycle_seen ||
                generation->sequence_seen)
                return fault(m, PGWT_MERGE_UNKNOWN_EVIDENCE, obs);
            /* Only this PID can conflict. Unrelated retained lifetimes must
             * not make ordinary creation scan the generation table. */
            size_t pid_bucket = (size_t)mix(r->pid) & (m->index_capacity - 1);
            for (size_t entry = m->pid_heads[pid_bucket]; entry;
                 entry = m->attachments[entry - 1].next_pid) {
                MERGE_WORK(STATE, 1);
                size_t j = entry - 1;
                const struct generation_state *other = m->planned[j] ?
                    &m->prospective[m->planned[j] - 1] : &m->attachments[j];
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
            if (r->fork_seed_proved) {
                generation->fork_seed_admitted = true;
                generation->creation = 2;
                generation->sequence_seen = true;
                a->sequence_proved = true;
                a->next_q = 1;
            }
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
            !dependency_available(m, r->dependency_id)) {
            wait_for_dependency(m, r->dependency_id);
            held = true;
        }
        if (m->has_last && order(m, &m->last, r) >= 0)
            return fault(m, PGWT_MERGE_SORT_ORDER_BREACH, obs);
        if (r->rank != PGWT_MERGE_TRANS) continue;
        if (m->config.mode == PGWT_MERGE_N) {
            /* contract §2: any same-generation cross-source TRANS/MARKER tie.
             * TRANS of one (t,pid,g) group are contiguous and source ordered,
             * so its first cross-source tie is between adjacent TRANS. */
            MERGE_WORK(QCOMPARE, previous_transition ? 1 : 0);
            const pgwt_merge_record *b = previous_transition;
            if (b && r->t == b->t && r->pid == b->pid && r->g == b->g &&
                r->source != b->source)
                return fault(m, PGWT_MERGE_AMBIGUOUS_TIE, obs);
            previous_transition = r;
            previous_slot = i;
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
    if (held) {
        m->held_cached = true;
        m->held_tail = tail;
        m->held_count = eligible;
        m->held_transition = previous_slot;
        return m->report.status;
    }
    bool new_frontier_epoch = !m->frontier_count ||
        m->frontiers[m->frontier_count - 1].epoch != m->report.epoch;
    if ((!m->report.has_frontier || W - 1 > m->report.frontier) &&
        new_frontier_epoch && m->frontier_count == m->config.frontier_capacity)
        return fault(m, PGWT_MERGE_ALLOCATION_FAIL, obs);
    for (size_t i = m->first; i != end; i = m->queue_nodes[i].next) {
        MERGE_WORK(QVISIT, 1);
        struct staged *v = &m->queue[i];
        pgwt_merge_output out = {.kind = v->text ? PGWT_MERGE_OUT_TEXT : PGWT_MERGE_OUT_DATA,
            .record = v->record, .epoch = m->report.epoch};
        struct generation_state *published = v->text ? NULL :
            attachment(m, &v->record);
        struct generation_state *planned = v->text ? NULL :
            &m->prospective[m->planned[published - m->attachments] - 1];
        MERGE_WORK(STATE, v->text ? 0 : 1);
        bool transition = !v->text && v->record.rank == PGWT_MERGE_TRANS;
        if (transition)
            out.prior_interval_certified = !planned->fork_seed_admitted || published->first_released;
        /* contract §3/§4: rejection is not output; post carries failure C,d. */
        if (!m->config.sink(m->config.sink_context, &out, &obs))
            return fault(m, PGWT_MERGE_WRITER_PUSH_FAIL, obs);
        /* DECISION 25c: retain the successful prefix even when a later
         * publication or counter checkpoint suspends this release. */
        if (published && v->record.rank == PGWT_MERGE_FORK)
            published->fork_seed_admitted = planned->fork_seed_admitted;
        if (transition) {
            published->first_released = true;
            planned->first_released = true;
        }
        if (!obs.counters.known) return fault(m, PGWT_MERGE_COUNTER_UNKNOWN, obs);
        if (!same_counters(obs.counters, m->baseline))
            return fault(m, counter_fault(obs.counters, m->observed), obs);
        if (!v->text) { m->last = v->record; m->has_last = true; }
    }
    for (size_t i = 0; i < m->touched_count; ++i) {
        size_t slot = m->touched[i];
        MERGE_WORK(STATE, 1);
        copy_generations(&m->attachments[slot], &m->prospective[i], 1);
    }
    /* Recycle only after every callback and checkpoint succeeded: fault()
     * must still see the entire batch if publication stops partway through. */
    for (size_t i = 0; i < eligible; ++i) queue_recycle_min(m);
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
                attachment(m, r)->lifecycle_seen = true;
            return fault(m, PGWT_MERGE_ALLOCATION_FAIL, obs);
        }
        if (in->kind == PGWT_MERGE_TEXT && r->dependency_id &&
            !dependency_available(m, r->dependency_id)) {
            if (m->dependency_count == m->config.dependency_capacity)
                return fault(m, PGWT_MERGE_ALLOCATION_FAIL, obs);
            m->dependencies[m->dependency_count++] = r->dependency_id;
            remember_dependency(m, m->dependencies[m->dependency_count - 1]);
        }
        size_t slot = m->free_slot;
        struct staged *v = &m->queue[slot];
        m->free_slot = m->queue_nodes[slot].next;
        ++m->count;
        MERGE_WORK(QMOVE, 1);
        v->text = in->kind == PGWT_MERGE_TEXT;
        v->record = *r;
        if (m->config.mode == PGWT_MERGE_N || r->rank != PGWT_MERGE_TRANS)
            v->record.q = 0;
        if (r->payload_size) memcpy(v->bytes, r->payload, r->payload_size);
        v->record.payload = v->bytes;
        m->root = queue_insert(m, m->root, slot, SIZE_MAX, SIZE_MAX);
        if (m->held_cached && v->text) ++m->held_count;
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
            if (!attachment(m, &r)) ++needed;
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
    size_t index_capacity = 1;
    while (index_capacity / 2 < c->attachment_capacity) {
        if (index_capacity > SIZE_MAX / 2)
            return create_fault(c, status, PGWT_MERGE_MALFORMED_ENVELOPE);
        index_capacity *= 2;
    }
    if (index_capacity > SIZE_MAX / sizeof(size_t))
        return create_fault(c, status, PGWT_MERGE_MALFORMED_ENVELOPE);
    size_t dependency_index_capacity = 1, waiting_index_capacity = 1;
    size_t dependency_capacity = c->dependency_capacity ? c->dependency_capacity : c->staged_capacity;
    while (dependency_index_capacity / 2 < dependency_capacity) {
        if (dependency_index_capacity > SIZE_MAX / 2)
            return create_fault(c, status, PGWT_MERGE_ALLOCATION_FAIL);
        dependency_index_capacity *= 2;
    }
    while (waiting_index_capacity / 2 < c->staged_capacity) waiting_index_capacity *= 2;
    if (dependency_index_capacity > SIZE_MAX / sizeof(struct dependency_entry) ||
        waiting_index_capacity > SIZE_MAX / sizeof(struct waiting_entry))
        return create_fault(c, status, PGWT_MERGE_ALLOCATION_FAIL);
    pgwt_merge *m = calloc(1, sizeof(*m));
    if (m) {
        m->config = *c;
        m->root = m->first = SIZE_MAX;
        m->index_capacity = index_capacity;
        m->dependency_index_capacity = dependency_index_capacity;
        m->waiting_index_capacity = waiting_index_capacity;
        m->dependency_index = calloc(dependency_index_capacity, sizeof(*m->dependency_index));
        m->waiting_index = calloc(waiting_index_capacity, sizeof(*m->waiting_index));
        m->waiting_slots = calloc(c->staged_capacity, sizeof(*m->waiting_slots));
        if (!m->config.frontier_capacity) m->config.frontier_capacity = c->staged_capacity;
        if (!m->config.dependency_capacity) m->config.dependency_capacity = c->staged_capacity;
        m->sources = calloc(c->source_count, sizeof(*m->sources));
        m->queue = calloc(c->staged_capacity, sizeof(*m->queue));
        /* queue_node is smaller than staged: the existing staged arithmetic
         * guard also proves this allocation's multiplication cannot overflow. */
        _Static_assert(sizeof(struct queue_node) <= sizeof(struct staged), "queue node size");
        m->queue_nodes = calloc(c->staged_capacity, sizeof(*m->queue_nodes));
        m->attachments = calloc(c->attachment_capacity ? c->attachment_capacity : 1,
                                sizeof(*m->attachments));
        size_t plan_capacity = c->attachment_capacity < c->staged_capacity ?
            c->attachment_capacity : c->staged_capacity;
        m->prospective = calloc(plan_capacity ? plan_capacity : 1, sizeof(*m->prospective));
        m->generation_index = calloc(index_capacity, sizeof(*m->generation_index));
        m->pid_heads = calloc(index_capacity, sizeof(*m->pid_heads));
        m->planned = calloc(c->attachment_capacity ? c->attachment_capacity : 1,
                            sizeof(*m->planned));
        /* Distinct touched generations cannot exceed the eligible DATA count. */
        m->touched = calloc(plan_capacity ? plan_capacity : 1, sizeof(*m->touched));
        m->frontiers = calloc(m->config.frontier_capacity, sizeof(*m->frontiers));
        m->dependencies = calloc(m->config.dependency_capacity, sizeof(*m->dependencies));
        if (m->queue) {
            /* contract §4: reserve every payload slot at creation. */
            unsigned char *bytes = calloc(c->staged_capacity,
                                           c->payload_capacity ? c->payload_capacity : 1);
            m->payload_storage = bytes;
            if (bytes) for (size_t i = 0; i < c->staged_capacity; ++i)
                m->queue[i].bytes = bytes + i * (c->payload_capacity ? c->payload_capacity : 1);
            if (m->queue_nodes) for (size_t i = 0; i < c->staged_capacity; ++i)
                m->queue_nodes[i].next = i + 1 < c->staged_capacity ? i + 1 : SIZE_MAX;
        }
    }
    if (!m || !m->sources || !m->queue || !m->queue_nodes || !m->attachments || !m->prospective ||
        !m->generation_index || !m->pid_heads || !m->planned || !m->touched ||
        !m->frontiers || !m->dependencies || !m->dependency_index ||
        !m->waiting_index || !m->waiting_slots ||
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
    for (size_t i = 0; i < c->attachment_count; ++i) {
        pgwt_merge_record r = {.pid = c->attachments[i].pid, .g = c->attachments[i].g};
        struct generation_state *a = remember_generation(m, &r);
        a->proof = c->attachments[i];
        a->sequence_seen = true;
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
    free(m->queue_nodes);
    free(m->sources);
    free(m->attachments);
    free(m->prospective);
    free(m->generation_index);
    free(m->pid_heads);
    free(m->planned);
    free(m->touched);
    free(m->frontiers);
    free(m->dependencies);
    free(m->dependency_index);
    free(m->waiting_index);
    free(m->waiting_slots);
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
