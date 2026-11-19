/* Opt-in receipt/attachment timing. Diagnostic state never drives capture. */
#ifndef PGWT_ATTACH_TIMELINE_H
#define PGWT_ATTACH_TIMELINE_H
#include "pg_wait_tracer.h"
#include "backend_status_layout.h"
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

#define PGWT_TIMELINE_SLOTS (4U * MAX_BACKENDS)
/* At most 64 row probes per lookup, including after 20k completed backends.
 * A create that finds no reusable slot in this window counts table_overflows;
 * diagnostic omissions must not turn into unbounded receive-to-handler work. */
#define PGWT_TIMELINE_MAX_PROBES 64U
enum pgwt_timeline_outcome {
    PGWT_TL_ATTACHED, PGWT_TL_NEVER_FIRED, PGWT_TL_STORED,
    PGWT_TL_NOT_STORED, PGWT_TL_UNKNOWN, PGWT_TL_OUTCOMES
};
enum pgwt_timeline_phase {
    PGWT_TL_BOOT_START, PGWT_TL_BOOT_END, PGWT_TL_WAIT_OPEN, PGWT_TL_WAIT_ENABLE
};
struct pgwt_timeline_row {
    uint32_t pid;
    unsigned slot; /* 0 empty, 1 active, 2 reusable completed slot */
    uint64_t cookie, fork_ns, rb_receive_ns, epoll_return_ns, batch_records_before;
    uint64_t handler_entry_ns, handler_exit_ns, bootstrap_open_start_ns;
    uint64_t bootstrap_open_ns, bootstrap_fire_ns, wait_open_ns, wait_enable_ns, exit_ns;
    uint64_t handler_entry_nvcsw, handler_entry_nivcsw, handler_exit_nvcsw, handler_exit_nivcsw;
    bool entry_usage_valid, exit_usage_valid, lost, attached_by_recovery;
    bool bootstrap_open_ok, wait_open_ok, wait_enable_ok;
    bool disposition_known, fired, stored;
};
struct pgwt_timeline_sample {
    uint64_t duration[4];
    unsigned valid;
    enum pgwt_timeline_outcome outcome;
    bool lost;
};
struct pgwt_attach_timeline {
    struct pgwt_timeline_row rows[PGWT_TIMELINE_SLOTS];
    struct pgwt_timeline_sample *samples;
    size_t count, capacity, pending;
    uint64_t completed, table_overflows, summary_dropped, replaced_pending;
    uint64_t epoll_return_ns, receive_ns, batch_records, records_before;
    uint64_t outer_loop_wakes, outer_loop_lifecycle_wakes, first_wake_ns, last_wake_ns;
    struct pgwt_pgbs_resolve_stats pgbs_resolve;
    bool wake_active, wake_has_lifecycle;
};

/* Existing daemon clock; no call is made while the diagnostic is off. */
uint64_t pgwt_debug_monotonic_ns(void);
static inline uint64_t pgwt_timeline_clock(void)
{
    int saved = errno;
    uint64_t ns = pgwt_debug_monotonic_ns();
    errno = saved;
    return ns;
}
static inline struct pgwt_timeline_row *pgwt_timeline_find(
    struct pgwt_attach_timeline *t, uint32_t pid, uint64_t cookie, bool create)
{
    if (!t || !pid) return NULL;
    size_t mask = PGWT_TIMELINE_SLOTS - 1;
    size_t slot = (size_t)((cookie ^ (cookie >> 32) ^ pid) *
                           UINT64_C(11400714819323198485)) & mask;
    struct pgwt_timeline_row *reuse = NULL;
    for (size_t i = 0; i < PGWT_TIMELINE_MAX_PROBES; ++i, slot = (slot + 1) & mask) {
        struct pgwt_timeline_row *r = &t->rows[slot];
        if (r->slot && r->pid == pid && r->cookie == cookie) return r;
        if (r->slot == 2 && !reuse) reuse = r;
        if (!r->slot) { if (!reuse) reuse = r; break; }
    }
    if (!create) return NULL;
    if (!reuse) { ++t->table_overflows; return NULL; }
    *reuse = (struct pgwt_timeline_row){.pid = pid, .cookie = cookie, .slot = 1};
    ++t->pending;
    return reuse;
}
/* A wake is any successful outer epoll return, including a timeout or a
 * no-sleep backlog return. L data is attributed once to that return, across
 * all consumes in the pass. Inner shutdown waits are deliberately excluded. */
static inline void pgwt_timeline_wake(struct pgwt_attach_timeline *t, int ready, uint64_t at)
{
    t->wake_active = ready >= 0;
    t->wake_has_lifecycle = false;
    if (!t->wake_active) return;
    if (!t->outer_loop_wakes) t->first_wake_ns = at;
    t->last_wake_ns = at;
    ++t->outer_loop_wakes;
}
static inline void pgwt_timeline_dequeue(struct pgwt_attach_timeline *t)
{
    if (t->wake_active && !t->wake_has_lifecycle) {
        ++t->outer_loop_lifecycle_wakes;
        t->wake_has_lifecycle = true;
    }
    t->receive_ns = pgwt_timeline_clock();
    t->records_before = t->batch_records++;
}
static inline void pgwt_timeline_receive(struct pgwt_attach_timeline *t,
    const struct pgwt_lifecycle_event *ev, uint64_t cookie)
{
    if (ev->type < PGWT_LIFECYCLE_FORK || ev->type > PGWT_LIFECYCLE_EXIT) return;
    struct pgwt_timeline_row *r = pgwt_timeline_find(t, ev->pid, cookie, true);
    if (!r) return;
    if (ev->type == PGWT_LIFECYCLE_FORK) {
        /* Default wire has only a PID. Do not merge two observed births. */
        if (r->slot == 2 || (r->fork_ns && r->fork_ns != ev->timestamp)) {
            if (r->slot == 1) ++t->replaced_pending;
            else ++t->pending;
            *r = (struct pgwt_timeline_row){.pid=ev->pid, .cookie=cookie, .slot=1};
        }
        r->fork_ns = ev->timestamp;
        r->rb_receive_ns = t->receive_ns;
        r->epoll_return_ns = t->epoll_return_ns;
        r->batch_records_before = t->records_before;
    } else if (r->slot == 1 && ev->type == PGWT_LIFECYCLE_INIT) {
        if (ev->timestamp) r->bootstrap_fire_ns = ev->timestamp;
    } else if (r->slot == 1 && ev->type == PGWT_LIFECYCLE_EXIT) {
        r->exit_ns = ev->timestamp;
        if (ev->exit_flags & PGWT_EXIT_DISPOSITION) {
            r->disposition_known = true;
            r->fired = (ev->exit_flags & PGWT_EXIT_BOOTSTRAP_FIRED) != 0;
            r->stored = (ev->exit_flags & PGWT_EXIT_INIT_STORED) != 0;
            if (r->stored && ev->addr) r->bootstrap_fire_ns = ev->addr;
        }
    }
}
static inline void pgwt_timeline_usage(struct pgwt_timeline_row *r, bool entry)
{
    int saved = errno;
#ifdef RUSAGE_THREAD
    struct rusage u;
    if (getrusage(RUSAGE_THREAD, &u) == 0) {
        if (entry) {
            r->handler_entry_nvcsw = (uint64_t)u.ru_nvcsw;
            r->handler_entry_nivcsw = (uint64_t)u.ru_nivcsw;
            r->entry_usage_valid = true;
        } else {
            r->handler_exit_nvcsw = (uint64_t)u.ru_nvcsw;
            r->handler_exit_nivcsw = (uint64_t)u.ru_nivcsw;
            r->exit_usage_valid = true;
        }
    }
#else
    (void)r; (void)entry; /* Host tests on macOS never substitute process usage. */
#endif
    errno = saved;
}
static inline void pgwt_timeline_handler(struct pgwt_attach_timeline *t,
    uint32_t pid, uint64_t cookie, bool entry)
{
    struct pgwt_timeline_row *r = pgwt_timeline_find(t, pid, cookie, false);
    if (!r || r->slot != 1) return;
    if (entry) r->handler_entry_ns = pgwt_timeline_clock();
    else r->handler_exit_ns = pgwt_timeline_clock();
    pgwt_timeline_usage(r, entry);
}
static inline void pgwt_timeline_phase(struct pgwt_attach_timeline *t,
    uint32_t pid, uint64_t cookie, enum pgwt_timeline_phase phase, bool ok)
{
    struct pgwt_timeline_row *r = pgwt_timeline_find(t, pid, cookie, true);
    if (!r || r->slot != 1) return;
    /* Keep the first success, or the first failed return if none succeeded.
     * Existing recovery may attempt attachment again; diagnostics never do. */
    uint64_t *ns = phase == PGWT_TL_BOOT_START ? &r->bootstrap_open_start_ns :
                   phase == PGWT_TL_BOOT_END ? &r->bootstrap_open_ns :
                   phase == PGWT_TL_WAIT_OPEN ? &r->wait_open_ns : &r->wait_enable_ns;
    bool succeeded = phase == PGWT_TL_BOOT_START ? true :
                     phase == PGWT_TL_BOOT_END ? r->bootstrap_open_ok :
                     phase == PGWT_TL_WAIT_OPEN ? r->wait_open_ok : r->wait_enable_ok;
    if (*ns && (succeeded || !ok)) return;
    *ns = pgwt_timeline_clock();
    if (phase == PGWT_TL_BOOT_END) r->bootstrap_open_ok = ok;
    if (phase == PGWT_TL_WAIT_OPEN) r->wait_open_ok = ok;
    if (phase == PGWT_TL_WAIT_ENABLE) r->wait_enable_ok = ok;
}
static inline void pgwt_timeline_init(struct pgwt_attach_timeline *t,
    uint32_t pid, uint64_t cookie, uint64_t init_ns)
{
    struct pgwt_timeline_row *r = pgwt_timeline_find(t, pid, cookie, false);
    if (r && r->slot == 1 && init_ns) r->bootstrap_fire_ns = init_ns;
}
static inline void pgwt_timeline_loss(struct pgwt_attach_timeline *t,
    uint32_t pid, uint64_t cookie)
{
    struct pgwt_timeline_row *r = pgwt_timeline_find(t, pid, cookie, true);
    if (r && r->slot == 1) r->lost = true;
}
/* Tick recovery has no lifecycle cookie. Credit the latest observed, still
 * active birth of this PID, never completed history or an unrelated tick row.
 * This bounded scan runs only for successful recovery with diagnostics on.
 * Keep outcome=attached: the real watchpoint is live in either attach path.
 * Do not invent phase timestamps when they were filed under another cookie. */
static inline void pgwt_timeline_recovered(struct pgwt_attach_timeline *t, uint32_t pid)
{
    struct pgwt_timeline_row *latest = NULL;
    for (size_t i = 0; i < PGWT_TIMELINE_SLOTS; ++i) {
        struct pgwt_timeline_row *r = &t->rows[i];
        if (r->slot == 1 && r->pid == pid && r->fork_ns && !r->exit_ns &&
            (!latest || r->fork_ns > latest->fork_ns))
            latest = r;
    }
    if (latest) latest->attached_by_recovery = true;
}
static inline const char *pgwt_timeline_outcome_name(enum pgwt_timeline_outcome o)
{
    const char *names[] = {"attached", "never_fired", "fired+stored", "fired+not_stored", "unknown"};
    return names[o];
}
static inline void pgwt_timeline_number(FILE *out, const char *name, uint64_t v, bool valid)
{
    fprintf(out, ",\"%s\":", name);
    if (valid) fprintf(out, "%" PRIu64, v); else fputs("null", out);
}
static inline const char *pgwt_timeline_json_uint(char buffer[24], uint64_t v, bool valid)
{
    if (!valid) return "null";
    snprintf(buffer, 24, "%" PRIu64, v);
    return buffer;
}
static inline const char *pgwt_timeline_json_bool(bool v, bool valid)
{
    return valid ? (v ? "true" : "false") : "null";
}
static inline void pgwt_timeline_finish(FILE *out, struct pgwt_attach_timeline *t,
    uint32_t pid, uint64_t cookie)
{
    struct pgwt_timeline_row *r = pgwt_timeline_find(t, pid, cookie, false);
    if (!r || r->slot != 1 || !r->exit_ns) return;
    int saved = errno;
    enum pgwt_timeline_outcome o = r->attached_by_recovery ||
        (r->wait_enable_ns && r->wait_enable_ok) ? PGWT_TL_ATTACHED :
        !r->disposition_known ? PGWT_TL_UNKNOWN : !r->fired ? PGWT_TL_NEVER_FIRED :
        r->stored ? PGWT_TL_STORED : PGWT_TL_NOT_STORED;
    /* Assemble on the stack: one stdio write per EXIT avoids dozens of
     * unbuffered stderr writes perturbing the next backend's receipt. */
    char numeric[16][24], line[2048];
#define PGWT_TL_VALUE(i, field, valid) pgwt_timeline_json_uint(numeric[i], r->field, (valid))
    const char *v[] = {
        PGWT_TL_VALUE(0, fork_ns, r->fork_ns != 0),
        PGWT_TL_VALUE(1, rb_receive_ns, r->rb_receive_ns != 0),
        PGWT_TL_VALUE(2, epoll_return_ns, r->epoll_return_ns != 0),
        PGWT_TL_VALUE(3, batch_records_before, r->rb_receive_ns != 0),
        PGWT_TL_VALUE(4, handler_entry_ns, r->handler_entry_ns != 0),
        PGWT_TL_VALUE(5, handler_exit_ns, r->handler_exit_ns != 0),
        PGWT_TL_VALUE(6, bootstrap_open_start_ns, r->bootstrap_open_start_ns != 0),
        PGWT_TL_VALUE(7, bootstrap_open_ns, r->bootstrap_open_ns != 0),
        PGWT_TL_VALUE(8, bootstrap_fire_ns, r->bootstrap_fire_ns != 0),
        PGWT_TL_VALUE(9, wait_open_ns, r->wait_open_ns != 0),
        PGWT_TL_VALUE(10, wait_enable_ns, r->wait_enable_ns != 0),
        PGWT_TL_VALUE(11, exit_ns, r->exit_ns != 0),
        PGWT_TL_VALUE(12, handler_entry_nvcsw, r->entry_usage_valid),
        PGWT_TL_VALUE(13, handler_entry_nivcsw, r->entry_usage_valid),
        PGWT_TL_VALUE(14, handler_exit_nvcsw, r->exit_usage_valid),
        PGWT_TL_VALUE(15, handler_exit_nivcsw, r->exit_usage_valid),
    };
#undef PGWT_TL_VALUE
    int length = snprintf(line, sizeof(line),
        "ATTACH-TIMELINE: {\"clock\":\"CLOCK_MONOTONIC\",\"pid\":%u,\"cookie\":%" PRIu64
        ",\"identity\":\"%s\",\"outcome\":\"%s\",\"lost\":%s"
        ",\"fork_ns\":%s,\"rb_receive_ns\":%s,\"epoll_return_ns\":%s,\"batch_records_before\":%s"
        ",\"handler_entry_ns\":%s,\"handler_exit_ns\":%s,\"bootstrap_open_start_ns\":%s"
        ",\"bootstrap_open_ns\":%s,\"bootstrap_fire_ns\":%s,\"wait_open_ns\":%s,\"wait_enable_ns\":%s,\"exit_ns\":%s"
        ",\"handler_entry_nvcsw\":%s,\"handler_entry_nivcsw\":%s,\"handler_exit_nvcsw\":%s,\"handler_exit_nivcsw\":%s"
        ",\"bootstrap_open_ok\":%s,\"wait_open_ok\":%s,\"wait_enable_ok\":%s}\n",
        pid, cookie, cookie ? "cookie" : "pid_only", pgwt_timeline_outcome_name(o), r->lost ? "true" : "false",
        v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15],
        pgwt_timeline_json_bool(r->bootstrap_open_ok, r->bootstrap_open_ns != 0),
        pgwt_timeline_json_bool(r->wait_open_ok, r->wait_open_ns != 0),
        pgwt_timeline_json_bool(r->wait_enable_ok, r->wait_enable_ns != 0));
    if (length > 0 && (size_t)length < sizeof(line)) fwrite(line, 1, (size_t)length, out);
    else fputs("ATTACH-TIMELINE-ERROR: line formatting failed\n", out);
    struct pgwt_timeline_sample s = {.outcome=o, .lost=r->lost};
    uint64_t from[] = {r->fork_ns, r->rb_receive_ns, r->handler_entry_ns, r->fork_ns};
    uint64_t to[] = {r->rb_receive_ns, r->handler_entry_ns,
                    r->bootstrap_open_ok ? r->bootstrap_open_ns : 0,
                    r->wait_enable_ok ? r->wait_enable_ns : 0};
    for (unsigned i = 0; i < 4; ++i) {
        if (from[i] && to[i] && to[i] >= from[i]) {
            s.duration[i] = to[i] - from[i]; s.valid |= 1U << i;
        }
    }
    if (t->count == t->capacity) {
        size_t cap = t->capacity ? t->capacity * 2 : 1024;
        void *p = cap > t->capacity && cap <= SIZE_MAX / sizeof(s)
                ? realloc(t->samples, cap * sizeof(s)) : NULL;
        if (p) { t->samples = p; t->capacity = cap; }
    }
    if (t->count < t->capacity) t->samples[t->count++] = s;
    else ++t->summary_dropped;
    ++t->completed; --t->pending; r->slot = 2;
    errno = saved;
}
static inline int pgwt_timeline_compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static inline uint64_t pgwt_timeline_quantile(const uint64_t *v, size_t n, unsigned p)
{
    /* Nearest rank ceil(n*p/100), without overflowing n*p. n > 0. */
    size_t rank = (n / 100) * p + ((n % 100) * p + 99) / 100;
    return v[rank - 1];
}
static inline void pgwt_timeline_summary(FILE *out, const struct pgwt_attach_timeline *t)
{
    const char *metrics[] = {"fork_to_receive_ns", "receive_to_handler_ns",
                            "handler_to_bootstrap_armed_ns", "fork_to_wait_enabled_ns"};
    uint64_t *values = t->count ? malloc(t->count * sizeof(*values)) : NULL;
    fprintf(out, "ATTACH-TIMELINE-STATS: {\"completed\":%" PRIu64 ",\"pending\":%zu,"
        "\"table_overflows\":%" PRIu64 ",\"summary_dropped\":%" PRIu64
        ",\"replaced_pending\":%" PRIu64 ",\"summary_allocation_ok\":%s"
        ",\"outer_loop_wakes\":%" PRIu64 ",\"outer_loop_lifecycle_wakes\":%" PRIu64
        ",\"outer_loop_elapsed_ns\":%" PRIu64
        ",\"pgbs_resolve_calls\":%" PRIu64 ",\"pgbs_resolve_misses\":%" PRIu64
        ",\"pgbs_resolve_total_ns\":%" PRIu64
        ",\"pgbs_resolve_extent_fallbacks\":%" PRIu64 "}\n",
        t->completed, t->pending, t->table_overflows, t->summary_dropped,
        t->replaced_pending, t->count && !values ? "false" : "true",
        t->outer_loop_wakes, t->outer_loop_lifecycle_wakes, t->last_wake_ns - t->first_wake_ns,
        t->pgbs_resolve.calls, t->pgbs_resolve.misses, t->pgbs_resolve.total_ns,
        t->pgbs_resolve.extent_fallbacks);
    if (t->count && !values) return;
    for (unsigned lost = 0; lost < 2; ++lost) {
        for (int outcome = -1; outcome < PGWT_TL_OUTCOMES; ++outcome) {
            size_t backends = 0;
            for (size_t j = 0; j < t->count; ++j)
                if ((!lost || t->samples[j].lost) &&
                    (outcome < 0 || t->samples[j].outcome == (enum pgwt_timeline_outcome)outcome)) ++backends;
            fprintf(out, "ATTACH-TIMELINE-SUMMARY: {\"population\":\"%s\",\"outcome\":\"%s\",\"backends\":%zu",
                lost ? "lost" : "all", outcome < 0 ? "all" : pgwt_timeline_outcome_name(outcome), backends);
            for (unsigned i = 0; i < 4; ++i) {
                size_t n = 0;
                for (size_t j = 0; j < t->count; ++j) {
                    const struct pgwt_timeline_sample *s = &t->samples[j];
                    if ((!lost || s->lost) && (outcome < 0 || s->outcome == (enum pgwt_timeline_outcome)outcome)
                        && (s->valid & (1U << i))) values[n++] = s->duration[i];
                }
                if (n) qsort(values, n, sizeof(*values), pgwt_timeline_compare);
                fprintf(out, ",\"%s\":{\"n\":%zu", metrics[i], n);
                pgwt_timeline_number(out, "p50", n ? pgwt_timeline_quantile(values, n, 50) : 0, n != 0);
                pgwt_timeline_number(out, "p90", n ? pgwt_timeline_quantile(values, n, 90) : 0, n != 0);
                pgwt_timeline_number(out, "p99", n ? pgwt_timeline_quantile(values, n, 99) : 0, n != 0);
                pgwt_timeline_number(out, "max", n ? values[n - 1] : 0, n != 0);
                fputc('}', out);
            }
            fputs("}\n", out);
        }
    }
    free(values);
}
#endif
