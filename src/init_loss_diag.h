/* Receipt-only INIT diagnostics. Never used for attachment or integrity. */
#ifndef PGWT_INIT_LOSS_DIAG_H
#define PGWT_INIT_LOSS_DIAG_H
#include "pg_wait_tracer.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define PGWT_INIT_DIAG_SLOTS (4U * MAX_BACKENDS)
_Static_assert((PGWT_INIT_DIAG_SLOTS & (PGWT_INIT_DIAG_SLOTS - 1)) == 0,
               "INIT diagnostic capacity must be a power of two");
struct pgwt_init_receipts {
    uint64_t cookie, fork_ts, init_record_ts, exit_record_ts, transitions;
    uint64_t fork_received_ns, init_received_ns, exit_received_ns, trace_exit_ts;
    uint32_t pid;
    bool attached, loss_pending;
    uint16_t exit_flags;
    uint64_t exit_init_ts;
};
struct pgwt_init_diagnostics {
    struct pgwt_init_receipts slots[PGWT_INIT_DIAG_SLOTS];
    uint64_t receipt_cookie;
    uint64_t overflows;
};
/* Open addressing, bounded by a fixed four-times-backend capacity. Empty
 * slots terminate a lookup. Deletion repairs the probe chain. */
static inline size_t pgwt_init_diag_hash(uint32_t pid, uint64_t cookie)
{
    uint64_t key = cookie ^ (cookie >> 32) ^ pid;
    return (size_t)(key * UINT64_C(11400714819323198485)) & (PGWT_INIT_DIAG_SLOTS - 1);
}
static inline struct pgwt_init_receipts *pgwt_init_diag_find(
    struct pgwt_init_diagnostics *d, uint32_t pid, uint64_t cookie, bool create)
{
    size_t slot = pgwt_init_diag_hash(pid, cookie);
    for (size_t n = 0; n < PGWT_INIT_DIAG_SLOTS; ++n) {
        struct pgwt_init_receipts *r = &d->slots[slot];
        if (r->pid == pid && r->cookie == cookie) return r;
        if (!r->pid) {
            if (!create) return NULL;
            *r = (struct pgwt_init_receipts){.pid = pid, .cookie = cookie};
            return r;
        }
        slot = (slot + 1) & (PGWT_INIT_DIAG_SLOTS - 1);
    }
    if (create) ++d->overflows;
    return NULL;
}
static inline void pgwt_init_diag_forget(struct pgwt_init_diagnostics *d,
                                         uint32_t pid, uint64_t cookie)
{
    struct pgwt_init_receipts *r = pgwt_init_diag_find(d, pid, cookie, false);
    if (!r) return;
    size_t hole = (size_t)(r - d->slots), pos = hole;
    d->slots[hole] = (struct pgwt_init_receipts){0};
    for (size_t n = 1; n < PGWT_INIT_DIAG_SLOTS; ++n) {
        pos = (pos + 1) & (PGWT_INIT_DIAG_SLOTS - 1);
        if (!d->slots[pos].pid) break;
        size_t home = pgwt_init_diag_hash(d->slots[pos].pid, d->slots[pos].cookie);
        size_t distance = (pos - home) & (PGWT_INIT_DIAG_SLOTS - 1);
        size_t to_hole = (hole - home) & (PGWT_INIT_DIAG_SLOTS - 1);
        if (to_hole < distance) {
            d->slots[hole] = d->slots[pos];
            d->slots[pos] = (struct pgwt_init_receipts){0};
            hole = pos;
        }
    }
}
/* Facts carried at BPF retirement, separate from accounting init_ts. */
static inline void pgwt_init_diag_disposition(FILE *out,
                                             const struct pgwt_init_receipts *r)
{
    if (!r || !r->exit_record_ts) {
        fputs(" exit_bootstrap_fired=pending exit_init_stored=pending"
              " exit_init_ts=pending exit_wp_ever_live=pending", out);
    } else if (!(r->exit_flags & PGWT_EXIT_DISPOSITION)) {
        fputs(" exit_bootstrap_fired=unknown exit_init_stored=unknown"
              " exit_init_ts=unknown exit_wp_ever_live=unknown", out);
    } else {
        fprintf(out, " exit_bootstrap_fired=%u exit_init_stored=%u"
            " exit_init_ts=%" PRIu64 " exit_wp_ever_live=%u",
            !!(r->exit_flags & PGWT_EXIT_BOOTSTRAP_FIRED),
            !!(r->exit_flags & PGWT_EXIT_INIT_STORED), r->exit_init_ts,
            !!(r->exit_flags & PGWT_EXIT_WP_EVER_LIVE));
    }
}
static inline void pgwt_init_diag_lifecycle(struct pgwt_init_diagnostics *d,
    const struct pgwt_lifecycle_event *ev, uint64_t received_ns)
{
    if (ev->type < PGWT_LIFECYCLE_FORK || ev->type > PGWT_LIFECYCLE_EXIT) return;
    struct pgwt_init_receipts *r = pgwt_init_diag_find(d, ev->pid, d->receipt_cookie, true);
    if (!r) return;
    if (ev->type == PGWT_LIFECYCLE_FORK) {
        /* The default wire has no cookie. A new FORK starts fresh receipt
         * history; pid_only remains explicit because delayed old records
         * cannot be disambiguated on that wire. */
        if (!r->cookie && r->fork_ts && r->fork_ts != ev->timestamp)
            *r = (struct pgwt_init_receipts){.pid = ev->pid};
        r->fork_ts = ev->timestamp; r->fork_received_ns = received_ns;
    }
    if (ev->type == PGWT_LIFECYCLE_INIT) {
        r->init_record_ts = ev->timestamp; r->init_received_ns = received_ns;
    }
    if (ev->type == PGWT_LIFECYCLE_EXIT) {
        r->exit_record_ts = ev->timestamp; r->exit_received_ns = received_ns;
        r->exit_flags = ev->exit_flags;
        r->exit_init_ts = ev->addr;
        if (r->loss_pending) {
            fprintf(stderr, "INIT-LOST-EVIDENCE-EXIT: clock=CLOCK_MONOTONIC"
                " pid=%u cookie=%" PRIu64 " exit_record_ts=%" PRIu64
                " exit_received_ns=%" PRIu64,
                r->pid, r->cookie, r->exit_record_ts, r->exit_received_ns);
            pgwt_init_diag_disposition(stderr, r);
            fputc('\n', stderr);
            r->loss_pending = false;
        }
    }
}
static inline void pgwt_init_diag_trace(struct pgwt_init_diagnostics *d,
    uint64_t cookie, const struct pgwt_trace_event *ev)
{
    if (d->overflows || PGWT_IS_MARKER(ev->old_event)) return;
    struct pgwt_init_receipts *r = pgwt_init_diag_find(d, ev->pid, cookie, true);
    if (r) {
        if (!r->attached) ++r->transitions;
        if (ev->new_event == PGWT_EVENT_EXIT) r->trace_exit_ts = ev->timestamp_ns;
    }
}
static inline void pgwt_init_diag_attached(struct pgwt_init_diagnostics *d,
                                          uint32_t pid, uint64_t cookie)
{
    struct pgwt_init_receipts *r = pgwt_init_diag_find(d, pid, cookie, false);
    if (r) r->attached = true;
}
/* Formatting and its clock read happen only on the loss path.
 * read_after_ns is an upper bound for completion of the last state lookup. */
static inline void pgwt_init_diag_print(FILE *out,
    struct pgwt_init_diagnostics *d, uint32_t pid, uint64_t cookie,
    const char *branch, uint64_t read_after_ns)
{
    struct pgwt_init_receipts *r = pgwt_init_diag_find(d, pid, cookie, false);
    bool known = r && !d->overflows;
    uint64_t bpf_exit_ts = r ? (r->exit_record_ts ? r->exit_record_ts : r->trace_exit_ts) : 0;
    const char *exit_source = r && r->exit_record_ts ? "lifecycle" : bpf_exit_ts ? "transition" : "unknown";
    if (r && r->exit_record_ts && r->trace_exit_ts && r->exit_record_ts != r->trace_exit_ts) {
        bpf_exit_ts = 0;
        exit_source = "conflict";
    }
    fprintf(out, "INIT-LOST-EVIDENCE: clock=CLOCK_MONOTONIC pid=%u cookie=%" PRIu64
        " identity=%s history=%s branch=%s fork=%s fork_ts=%" PRIu64
        " fork_received_ns=%" PRIu64 " init_record=%s init_record_ts=%" PRIu64
        " init_received_ns=%" PRIu64 " exit_record=%s exit_record_ts=%" PRIu64
        " exit_received_ns=%" PRIu64 " transitions_received=",
        pid, cookie, cookie ? "cookie" : "pid_only", known ? "tracked" : "incomplete",
        branch, r && r->fork_ts ? "delivered" : known ? "not_received" : "unknown",
        r ? r->fork_ts : 0, r ? r->fork_received_ns : 0,
        r && r->init_record_ts ? "delivered" : known ? "not_received" : "unknown",
        r ? r->init_record_ts : 0, r ? r->init_received_ns : 0,
        r && r->exit_record_ts ? "delivered" : known ? "not_received" : "unknown",
        r ? r->exit_record_ts : 0, r ? r->exit_received_ns : 0);
    if (known) fprintf(out, "%" PRIu64, r->transitions);
    else fputs("unknown", out);
    fprintf(out, " bpf_exit_ts=%" PRIu64 " bpf_exit_source=%s state_read_after_ns=%" PRIu64
        " exit_to_read_ns=", bpf_exit_ts, exit_source, read_after_ns);
    if (bpf_exit_ts && read_after_ns >= bpf_exit_ts)
        fprintf(out, "%" PRIu64, read_after_ns - bpf_exit_ts);
    else fputs("unknown", out);
    fprintf(out, " history_overflows=%" PRIu64, d->overflows);
    pgwt_init_diag_disposition(out, r);
    fputc('\n', out);
    /* Preserve the join even when retirement was observed before any wire
     * record. A default-ring PID has no lifetime identity, so cannot join a
     * late EXIT safely. Capacity failures remain diagnostic unknowns. */
    if (cookie) {
        if (!r) r = pgwt_init_diag_find(d, pid, cookie, true);
        if (r) r->loss_pending = !r->exit_record_ts;
    }
}
#endif
