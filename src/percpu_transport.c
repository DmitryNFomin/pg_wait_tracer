#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Exact transport reader. Kernel operations live behind pgwt_transport_io. */
#include "percpu_transport.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>

struct queued_wire {
    struct pgwt_wire_record record;
    unsigned char payload[272];
    uint64_t arrived;
};
struct pgwt_percpu_transport {
    void *platform;
    struct pgwt_transport_io io;
    uint32_t *cpus;
    size_t ncpu, capacity, count;
    uint64_t *positions, *seq, *targets;
    struct queued_wire *queue;
    bool *acks;
    bool requested, fencing, suspended, finished, draining;
    bool prepared, configured, opened, serviced;
    bool e_seen, l_seen;
    uint32_t round;
    ma_snapshot snapshot;
    uint64_t max_held, lost_total, last_record_at, last_service_at;
    bool lost_valid;
};
struct pgwt_transport_perf_attributes pgwt_transport_perf_attributes(size_t bytes)
{
    size_t threshold = bytes / 8;
    return (struct pgwt_transport_perf_attributes){
        .type = 1, .config = 10, .sample_type = UINT64_C(1) << 10,
        .sample_period = 1, .watermark = true,
        .wakeup_watermark = (uint32_t)(threshold > 65536 ? 65536 : threshold)};
}
uint64_t pgwt_transport_service_interval_ns(void) { return UINT64_C(10000000); }
bool pgwt_transport_service_due(uint64_t now, uint64_t previous, bool serviced)
{
    uint64_t interval = pgwt_transport_service_interval_ns();
    return !serviced || now < previous || now / interval != previous / interval;
}
static uint64_t read_le(const unsigned char *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= (uint64_t)p[i] << (i * 8);
    return v;
}
static size_t cpu_index(const uint32_t *ids, size_t n, uint32_t cpu)
{
    for (size_t i = 0; i < n; ++i) if (ids[i] == cpu) return i;
    return n;
}
int pgwt_transport_decode(const uint32_t *cpus, size_t ncpu, uint32_t dest,
                          uint64_t position, const void *bytes, size_t size,
                          struct pgwt_wire_record *out)
{
    if (!cpus || !ncpu || !bytes || !out || !position || size < 32 || dest > 2)
        return -EINVAL;
    const unsigned char *p = bytes;
    size_t len = (size_t)read_le(p + 4, 2);
    uint32_t cpu = (uint32_t)read_le(p + 6, 2);
    size_t i = cpu_index(cpus, ncpu, cpu);
    /* Mode N: q and HAS_Q are reserved, never producer evidence. */
    if (read_le(p, 2) != PGWT_WIRE_VERSION || p[3] || read_le(p + 24, 8) ||
        i == ncpu || len != size - 32 || !read_le(p + 8, 8)) return -EINVAL;
    ma_envelope e = {.version = 1, .origin = MA_EXACT, .position = position,
        .source = dest == PGWT_WIRE_DEST_CPU ? i : ncpu + dest - 1,
        .cookie = read_le(p + 16, 8), .bytes = p + 32, .size = len};
    e.cookie_present = e.cookie != 0;
    switch (p[2]) {
    case PGWT_WIRE_TRACE:
        if (len != 48 || dest == PGWT_WIRE_DEST_L) return -EINVAL;
        e.kind = MA_TRACE;
        break;
    case PGWT_WIRE_LIFECYCLE:
        if (len != 24 || dest != PGWT_WIRE_DEST_L ||
            read_le(p + 32, 4) < 1 || read_le(p + 32, 4) > 3) return -EINVAL;
        e.kind = MA_LIFECYCLE;
        break;
    case PGWT_WIRE_TEXT:
        if (len != 272 || dest != PGWT_WIRE_DEST_L || read_le(p + 32, 4) != 4)
            return -EINVAL;
        e.kind = MA_TEXT;
        break;
    case PGWT_WIRE_BOUNDARY:
        if (len != 16 || read_le(p + 44, 4) != dest) return -EINVAL;
        e.kind = MA_BOUNDARY;
        e.w = read_le(p + 32, 8);
        e.boundary = dest == PGWT_WIRE_DEST_CPU ? MA_IDLE_PERF : MA_RING_BOUNDARY;
        e.bytes = NULL; e.size = 0;
        break;
    default: return -EINVAL;
    }
    *out = (struct pgwt_wire_record){.envelope = e, .cpu = cpu, .dest = dest,
        .seq = read_le(p + 8, 8),
        .round = e.kind == MA_BOUNDARY ? (uint32_t)read_le(p + 40, 4) : 0};
    return 0;
}
static uint64_t observed(struct pgwt_percpu_transport *t) { return t->io.now(t->io.ctx); }
static int counters(struct pgwt_percpu_transport *t)
{
    memset(&t->snapshot, 0, sizeof(t->snapshot));
    if (t->io.counters(t->io.ctx, &t->snapshot) < 0) t->snapshot.valid = false;
    return 0;
}
static int fault(struct pgwt_percpu_transport *t, pgwt_merge_reason reason)
{
    t->suspended = true;
    counters(t);
    return t->io.fault(t->io.ctx, reason, observed(t), &t->snapshot);
}
struct pgwt_percpu_transport *pgwt_transport_create(
    const uint32_t *cpus, size_t ncpu, size_t capacity, const struct pgwt_transport_io *io)
{
    if (!cpus || !ncpu || !capacity || ncpu > 65536 || capacity > SIZE_MAX / sizeof(struct queued_wire) ||
        !io || !io->now || !io->control || !io->wake || !io->attempts ||
        !io->counters || !io->drain || !io->envelope || !io->fault || !io->cpu_set_unchanged)
        return NULL;
    for (size_t i = 0; i < ncpu; ++i)
        if (cpus[i] > UINT16_MAX || (i && cpus[i] <= cpus[i-1])) return NULL;
    struct pgwt_percpu_transport *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->cpus = malloc(ncpu * sizeof(*cpus));
    t->positions = calloc(ncpu + 2, sizeof(uint64_t));
    t->seq = calloc(ncpu * 3, sizeof(uint64_t));
    t->targets = calloc(ncpu * 3, sizeof(uint64_t));
    t->acks = calloc(ncpu, sizeof(bool));
    t->queue = calloc(capacity, sizeof(*t->queue));
    if (!t->cpus || !t->positions || !t->seq || !t->targets || !t->acks || !t->queue) {
        pgwt_transport_destroy(t); return NULL;
    }
    memcpy(t->cpus, cpus, ncpu * sizeof(*cpus));
    t->ncpu = ncpu; t->capacity = capacity; t->io = *io; t->round = 1; t->lost_valid = true;
    t->last_record_at = observed(t);
    counters(t);
    return t;
}
int pgwt_transport_record(struct pgwt_percpu_transport *t, uint32_t dest,
                           const void *bytes, size_t size)
{
    if (!t || t->finished) return -EINVAL;
    struct pgwt_wire_record r;
    if (bytes && size >= 32 && read_le(bytes, 2) == PGWT_WIRE_VERSION &&
        cpu_index(t->cpus, t->ncpu, (uint32_t)read_le((const unsigned char *)bytes + 6, 2)) == t->ncpu) {
        fprintf(stderr, "exact-perf: CPU outside the fixed set; suspending (hotplug unsupported)\n");
        return fault(t, PGWT_MERGE_UNKNOWN_EVIDENCE);
    }
    /* Decode twice only to discover the source before assigning its position. */
    if (pgwt_transport_decode(t->cpus, t->ncpu, dest, 1, bytes, size, &r) < 0)
        return fault(t, PGWT_MERGE_MALFORMED_ENVELOPE);
    size_t source = r.envelope.source;
    if (t->positions[source] == UINT64_MAX) return fault(t, PGWT_MERGE_MALFORMED_ENVELOPE);
    r.envelope.position = ++t->positions[source];
    size_t i = cpu_index(t->cpus, t->ncpu, r.cpu), k = i * 3 + dest;
    if (r.seq != t->seq[k] + 1 || t->seq[k] == UINT64_MAX) {
        t->seq[k] = r.seq;
        return fault(t, PGWT_MERGE_SEQUENCE_GAP);
    }
    t->seq[k] = r.seq;
    uint64_t now = observed(t);
    t->last_record_at = now;
    if (t->io.received) {
        int rc = t->io.received(t->io.ctx, &r.envelope, now);
        if (rc < 0) return fault(t, rc == -ENOMEM ? PGWT_MERGE_ALLOCATION_FAIL :
            rc == -EINVAL ? PGWT_MERGE_MALFORMED_ENVELOPE : PGWT_MERGE_WRITER_PUSH_FAIL);
    }
    if (t->suspended) return 0; /* consume/account, never automatically rearm */
    if (r.envelope.kind == MA_BOUNDARY) {
        if (t->requested && !t->fencing && r.round == t->round) {
            if (dest == PGWT_WIRE_DEST_E) t->e_seen = true;
            if (dest == PGWT_WIRE_DEST_L) t->l_seen = true;
        }
        if (t->fencing && dest == PGWT_WIRE_DEST_CPU && r.round > t->round)
            t->acks[i] = true;
        /* Unrequested/stale boundaries are not completeness evidence. Their
         * positions still count. Never pass them through to the core. */
        if (!t->requested || (dest == 0 ? !t->fencing || r.round <= t->round :
                                                      r.round != t->round)) return 0;
    }
    if (t->count == t->capacity) return fault(t, PGWT_MERGE_ALLOCATION_FAIL);
    struct queued_wire *q = &t->queue[t->count++];
    q->record = r; q->arrived = now;
    if (r.envelope.size) {
        memcpy(q->payload, r.envelope.bytes, r.envelope.size);
        q->record.envelope.bytes = q->payload;
    }
    return 0;
}
int pgwt_transport_ring_record(struct pgwt_percpu_transport *t, uint32_t dest,
                               const void *bytes, size_t size, size_t *consumed, size_t limit)
{
    if (!consumed) return -EINVAL;
    int rc = pgwt_transport_record(t, dest, bytes, size);
    ++*consumed;
    if (rc < 0) return rc;
    return limit && *consumed >= limit ? -EAGAIN : 0;
}
int pgwt_transport_ring_result(int rc, size_t consumed, size_t limit)
{
    return rc == -EAGAIN && limit && consumed >= limit ? (int)consumed :
           rc < 0 ? rc : (int)consumed;
}
int pgwt_transport_perf(struct pgwt_percpu_transport *t, uint32_t cpu,
                        const void *bytes, size_t size)
{
    if (!t || !bytes || size < 4 + 32) return t ? fault(t, PGWT_MERGE_MALFORMED_ENVELOPE) : -EINVAL;
    const unsigned char *p = bytes;
    size_t wire_size = 32 + (size_t)read_le(p + 8, 2);
    size_t padded = (wire_size + 4 + 7) & ~(size_t)7;
    if (size != padded || read_le(p, 4) != padded - 4 || read_le(p + 10, 2) != cpu)
        return fault(t, PGWT_MERGE_MALFORMED_ENVELOPE);
    return pgwt_transport_record(t, PGWT_WIRE_DEST_CPU, p + 4, wire_size);
}
int pgwt_transport_lost(struct pgwt_percpu_transport *t, uint32_t cpu, uint64_t n)
{
    if (!t) return -EINVAL;
    if (cpu_index(t->cpus, t->ncpu, cpu) == t->ncpu) return fault(t, PGWT_MERGE_UNKNOWN_EVIDENCE);
    if (!n) return 0;
    if (UINT64_MAX - t->lost_total < n) t->lost_valid = false;
    else t->lost_total += n;
    t->suspended = true; counters(t);
    ma_envelope e = {.version = 1, .kind = MA_LOSS, .lost = n,
        .source = cpu_index(t->cpus, t->ncpu, cpu)};
    return t->io.envelope(t->io.ctx, &e, observed(t), &t->snapshot);
}
static int request(struct pgwt_percpu_transport *t, bool fence)
{
    if (fence && t->round == UINT32_MAX) return fault(t, PGWT_MERGE_UNKNOWN_EVIDENCE);
    struct pgwt_transport_ctl c = {.round = t->round + (fence ? 1U : 0U),
                                    .el_request = fence ? 0 : 1};
    if (t->io.control(t->io.ctx, &c) < 0) return fault(t, PGWT_MERGE_UNKNOWN_EVIDENCE);
    for (size_t i = 0; i < t->ncpu; ++i)
        if (t->io.wake(t->io.ctx, t->cpus[i]) < 0) return fault(t, PGWT_MERGE_UNKNOWN_EVIDENCE);
    return 0;
}
static int release_queue(struct pgwt_percpu_transport *t, bool checkpoint)
{
    ma_envelope check = {.version = 1, .kind = MA_CHECK};
    int rc = checkpoint ? t->io.envelope(t->io.ctx, &check, observed(t), &t->snapshot) : 0;
    if (rc < 0) return rc;
    size_t n = t->count;
    for (size_t j = 0; j < n; ++j) {
        uint64_t now = observed(t);
        if (now >= t->queue[j].arrived && now - t->queue[j].arrived > t->max_held)
            t->max_held = now - t->queue[j].arrived;
        rc = t->io.envelope(t->io.ctx, &t->queue[j].record.envelope, now, &t->snapshot);
        if (rc < 0) return rc;
    }
    /* Callbacks cannot reenter the transport reader. */
    t->count = 0;
    return 0;
}
/* Completion is progress work, not a periodic duty. After the CPU acks,
 * retry every loop pass: a non-empty bounded drain proves nothing, and
 * waiting another service slot can multiply its delay under sustained load. */
static int complete_round(struct pgwt_percpu_transport *t)
{
    if (t->suspended || t->draining) return 0;
    if (t->io.suspended && t->io.suspended(t->io.ctx)) {
        t->suspended = true;
        return 0;
    }
    if (!t->fencing) return 0;
    for (size_t i = 0; i < t->ncpu; ++i) if (!t->acks[i]) return 0;
    t->draining = true;
    int drained = t->io.drain(t->io.ctx);
    t->draining = false;
    if (drained < 0) return fault(t, PGWT_MERGE_UNKNOWN_EVIDENCE);
    if (!drained) return 0;
    for (size_t k = 0; k < t->ncpu * 3; ++k)
        if (t->seq[k] < t->targets[k]) return fault(t, PGWT_MERGE_SEQUENCE_GAP);
    /* Completion fence: this reread MUST follow every CPU's consumed ack. */
    counters(t);
    int rc = release_queue(t, true);
    if (rc < 0) return rc;
    t->requested = t->fencing = t->e_seen = t->l_seen = false;
    memset(t->acks, 0, t->ncpu * sizeof(bool));
    ++t->round;
    return 0;
}
int pgwt_transport_poll(struct pgwt_percpu_transport *t, bool waiting)
{
    if (!t || t->finished) return -EINVAL;
    if (t->io.suspended && t->io.suspended(t->io.ctx)) t->suspended = true;
    if (!t->suspended && t->io.cpu_set_unchanged(t->io.ctx) != 1) return fault(t, PGWT_MERGE_UNKNOWN_EVIDENCE);
    if (t->draining) return 0;
    uint64_t now = observed(t);
    for (size_t j = 0; j < t->count; ++j)
        if (now >= t->queue[j].arrived && now - t->queue[j].arrived > t->max_held)
            t->max_held = now - t->queue[j].arrived;
    if (t->suspended) {
        counters(t);
        ma_envelope e = {.version = 1, .kind = MA_CHECK};
        return t->io.envelope(t->io.ctx, &e, now, &t->snapshot);
    }
    if (!t->requested && (t->count || waiting)) {
        t->requested = true;
        int rc = request(t, false);
        if (rc < 0 || t->suspended) return rc;
    }
    if (!t->requested) return 0; /* idle means no helper work */
    if (!t->fencing && t->e_seen && t->l_seen) {
        /* A lower bound captured before the wake: do NOT compare current
         * growing producer totals with a partially consumed live stream. */
        if (t->io.attempts(t->io.ctx, t->targets, t->ncpu * 3) < 0)
            return fault(t, PGWT_MERGE_COUNTER_UNKNOWN);
        for (size_t k = 0; k < t->ncpu * 3; ++k)
            if (t->targets[k] < t->seq[k]) return fault(t, PGWT_MERGE_COUNTER_UNKNOWN);
        t->fencing = true;
        int rc = request(t, true);
        if (rc < 0 || t->suspended) return rc;
    }
    return complete_round(t);
}

int pgwt_transport_finish(struct pgwt_percpu_transport *t)
{
    if (!t) return -EINVAL;
    if (t->finished) return 0;
    /* Producers are still attached. Drain CPU perf AND E/L before starting
     * the final completion round, then continue bounded passes for its acks.
     * Live producers, BUSY reservations or missing acks cannot hang shutdown:
     * any unfinished suffix is explicitly VOID, including unread raw data. */
    int rc = 0;
    uint64_t started = observed(t);
    bool done = false;
    for (unsigned pass = 0; pass < 256; ++pass) {
        int drained = t->io.drain(t->io.ctx);
        if (drained < 0) { rc = fault(t, PGWT_MERGE_UNKNOWN_EVIDENCE); done = true; break; }
        if (!t->suspended) {
            rc = pgwt_transport_poll(t, t->io.waiting && t->io.waiting(t->io.ctx));
            if (rc < 0) break;
        }
        if (drained && (t->suspended || (!t->count && !t->requested &&
            !(t->io.waiting && t->io.waiting(t->io.ctx))))) { done = true; break; }
        uint64_t now = observed(t);
        if (now < started || now - started >= UINT64_C(100000000)) break;
        if (t->io.wait && t->io.wait(t->io.ctx) < 0) break;
    }
    if (!done && rc >= 0) rc = fault(t, PGWT_MERGE_UNKNOWN_EVIDENCE);
    if (done && !t->suspended) {
        /* Also account for a missing terminal attempt with no later record. */
        if (t->io.attempts(t->io.ctx, t->targets, t->ncpu * 3) < 0)
            rc = fault(t, PGWT_MERGE_COUNTER_UNKNOWN);
        else for (size_t k = 0; k < t->ncpu * 3; ++k)
            if (t->targets[k] != t->seq[k]) { rc = fault(t, PGWT_MERGE_SEQUENCE_GAP); break; }
    }
    t->finished = true;
    return rc;
}

#ifdef PGWT_TRANSPORT_HOST_TEST
void pgwt_transport_replay_position(struct pgwt_percpu_transport *t, size_t source, uint64_t position)
{
    t->positions[source] = position - 1;
    t->requested = t->fencing = true;
    t->suspended = false;
}
int pgwt_transport_replay_release(struct pgwt_percpu_transport *t)
{
    counters(t);
    return release_queue(t, false);
}
#endif
bool pgwt_transport_lost_total(const struct pgwt_percpu_transport *t, uint64_t *n)
{
    if (!t || !n) return false;
    *n = t->lost_total; return t->lost_valid;
}
uint64_t pgwt_transport_max_held_ns(const struct pgwt_percpu_transport *t)
{ return t ? t->max_held : 0; }
size_t pgwt_transport_pending_records(const struct pgwt_percpu_transport *t)
{ return t ? t->count : 0; }
bool pgwt_transport_silent(const struct pgwt_percpu_transport *t, uint64_t now, uint64_t interval)
{ return t && now >= t->last_record_at && now - t->last_record_at >= interval; }
void pgwt_transport_destroy(struct pgwt_percpu_transport *t)
{
    if (!t) return;
    free(t->cpus); free(t->positions); free(t->seq); free(t->targets);
    free(t->acks); free(t->queue); free(t);
}

#include "daemon.h"
#include <stdio.h>
#if defined(__linux__) && !defined(PGWT_TRANSPORT_HOST_TEST)
/* Linux I/O for the portable reader. No skeleton field depends on A1 names:
 * map presence/types and the getppid helper section are checked explicitly. */
#include "daemon.h"
#include "pg_wait_tracer.skel.h"
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/perf_event.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

struct pinned_helper {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint32_t cpu;
    size_t mask_size;
    bool initialized, started, ready, wake, stop;
    int error;
};
struct ring_context {
    struct pgwt_daemon *d;
    uint32_t dest;
    size_t consumed, limit;
};
struct linux_transport {
    struct pgwt_daemon *d;
    struct perf_buffer **perf;
    struct ring_buffer *rings[2];
    struct ring_context ring_ctx[2];
    struct pinned_helper *helpers;
    struct bpf_link *helper_link;
    size_t ncpu;
    int possible, epoll_fd, attempts_fd, ctl_fd, callback_error;
    bool topology_reported;
};
static struct linux_transport *linux_state(const struct pgwt_daemon *d)
{ return d && d->percpu_transport ? d->percpu_transport->platform : NULL; }
/* sysfs online CPU membership is authoritative, not the daemon's affinity. */
static int online_cpus(uint32_t *out, size_t cap)
{
    FILE *f = fopen("/sys/devices/system/cpu/online", "r");
    if (!f) return -errno;
    char text[65536];
    if (!fgets(text, sizeof(text), f)) { fclose(f); return -EIO; }
    fclose(f);
    if (!strchr(text, '\n')) return -EOVERFLOW;
    char *p = text, *end;
    size_t n = 0;
    while (*p && *p != '\n') {
        errno = 0;
        unsigned long first = strtoul(p, &end, 10), last = first;
        if (errno || end == p || first > UINT16_MAX) return -EINVAL;
        p = end;
        if (*p == '-') {
            ++p; last = strtoul(p, &end, 10);
            if (errno || end == p || last < first || last > UINT16_MAX) return -EINVAL;
            p = end;
        }
        for (unsigned long i = first; i <= last; ++i) {
            if (n == cap || (n && out[n-1] >= i)) return -EOVERFLOW;
            out[n++] = (uint32_t)i;
        }
        if (*p == ',') { ++p; if (!*p || *p == '\n') return -EINVAL; }
        else if (*p && *p != '\n') return -EINVAL;
    }
    return n ? (int)n : -EINVAL;
}
static uint64_t linux_now(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}
static int linux_topology(void *ctx)
{
    struct linux_transport *l = ctx;
    uint32_t *ids = calloc((size_t)l->possible, sizeof(*ids));
    if (!ids) return -ENOMEM;
    int n = online_cpus(ids, (size_t)l->possible);
    bool same = n == (int)l->ncpu && !memcmp(ids, l->d->percpu_transport->cpus, l->ncpu * sizeof(*ids));
    free(ids);
    for (size_t i = 0; i < l->ncpu; ++i) {
        pthread_mutex_lock(&l->helpers[i].mutex);
        if (l->helpers[i].error) same = false;
        pthread_mutex_unlock(&l->helpers[i].mutex);
    }
    if (!same && !l->topology_reported) {
        fprintf(stderr, "exact-perf: CPU set changed or pinned helper failed; capture suspended; hotplug unsupported\n");
        l->topology_reported = true;
    }
    return same ? 1 : 0;
}
static int linux_counters(void *ctx, ma_snapshot *s)
{
    struct linux_transport *l = ctx;
    uint64_t now;
    return pgwt_merge_capture_snapshot(l->d, s, &now);
}
static int linux_attempts(void *ctx, uint64_t *out, size_t count)
{
    struct linux_transport *l = ctx;
    if (count != l->ncpu * 3 || l->attempts_fd < 0) return -EINVAL;
    uint64_t *v = calloc((size_t)l->possible, sizeof(*v));
    if (!v) return -ENOMEM;
    int rc = 0;
    for (uint32_t dest = 0; dest < 3; ++dest) {
        if (bpf_map_lookup_elem(l->attempts_fd, &dest, v)) { rc = -errno; break; }
        for (size_t i = 0; i < l->ncpu; ++i) out[i * 3 + dest] = v[l->d->percpu_transport->cpus[i]];
    }
    free(v); return rc;
}
static int linux_control(void *ctx, const struct pgwt_transport_ctl *ctl)
{
    struct linux_transport *l = ctx;
    uint32_t zero = 0;
    return bpf_map_update_elem(l->ctl_fd, &zero, ctl, BPF_ANY) ? -errno : 0;
}
static int linux_wake(void *ctx, uint32_t cpu)
{
    struct linux_transport *l = ctx;
    size_t i = cpu_index(l->d->percpu_transport->cpus, l->ncpu, cpu);
    if (i == l->ncpu) return -EINVAL;
    struct pinned_helper *h = &l->helpers[i];
    pthread_mutex_lock(&h->mutex);
    h->wake = true; pthread_cond_signal(&h->cond);
    int err = h->error;
    pthread_mutex_unlock(&h->mutex);
    return -err;
}
static int linux_received(void *ctx, const ma_envelope *e, uint64_t d)
{ return pgwt_capture_received(((struct linux_transport *)ctx)->d->merge_capture, e, d); }
static bool linux_suspended(void *ctx)
{ return pgwt_capture_report(((struct linux_transport *)ctx)->d->merge_capture).status != PGWT_MERGE_OK; }
static bool linux_waiting(void *ctx)
{ return pgwt_capture_waiting(((struct linux_transport *)ctx)->d->merge_capture); }
static int linux_wait(void *ctx)
{
    struct epoll_event events[1];
    int rc = epoll_wait(((struct linux_transport *)ctx)->epoll_fd, events, 1, 1);
    return rc < 0 && errno != EINTR ? -errno : 0;
}
static int linux_envelope(void *ctx, const ma_envelope *e, uint64_t d, ma_snapshot *s)
{
    struct pgwt_merge_capture *c = ((struct linux_transport *)ctx)->d->merge_capture;
    int rc = pgwt_capture_envelope(c, e, d, s);
    *s = pgwt_capture_last_snapshot(c);
    return rc;
}
static int linux_fault(void *ctx, pgwt_merge_reason r, uint64_t d, const ma_snapshot *s)
{
    struct linux_transport *l = ctx;
    return l->d->merge_capture ? pgwt_capture_fault(l->d->merge_capture, r, d, s) : -EIO;
}
static void *helper_loop(void *ctx)
{
    struct pinned_helper *h = ctx;
    cpu_set_t *mask = calloc(1, h->mask_size);
    sigset_t signals;
    sigemptyset(&signals); sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM);
    int mask_error = pthread_sigmask(SIG_BLOCK, &signals, NULL);
    int error = mask_error ? mask_error : mask ? 0 : ENOMEM;
    if (mask) {
        CPU_SET_S(h->cpu, h->mask_size, mask);
        if (sched_setaffinity(0, h->mask_size, mask)) error = errno;
        if (!error && sched_getcpu() != (int)h->cpu) error = EINVAL;
    }
    int policy; struct sched_param param;
    if (!error) {
        int rc = pthread_getschedparam(pthread_self(), &policy, &param);
        if (rc || policy != SCHED_OTHER) error = rc ? rc : EINVAL;
    }
    pthread_mutex_lock(&h->mutex);
    h->error = error; h->ready = true; pthread_cond_signal(&h->cond);
    while (!h->stop && !h->error) {
        while (!h->wake && !h->stop) pthread_cond_wait(&h->cond, &h->mutex);
        if (h->stop) break;
        h->wake = false;
        /* Verify the pin on every requested syscall. No timed/spurious wakes. */
        CPU_ZERO_S(h->mask_size, mask);
        if (sched_getaffinity(0, h->mask_size, mask) ||
            CPU_COUNT_S(h->mask_size, mask) != 1 || !CPU_ISSET_S(h->cpu, h->mask_size, mask) ||
            sched_getcpu() != (int)h->cpu) { h->error = EINVAL; break; }
        if (syscall(SYS_getppid) < 0) { h->error = errno; break; }
    }
    pthread_mutex_unlock(&h->mutex);
    free(mask); return NULL;
}
static int receive_ring(void *ctx, void *bytes, size_t size)
{
    struct ring_context *r = ctx;
    int rc = pgwt_transport_ring_record(r->d->percpu_transport, r->dest, bytes, size,
                                       &r->consumed, r->limit);
    if (rc < 0 && !(rc == -EAGAIN && r->limit && r->consumed >= r->limit))
        linux_state(r->d)->callback_error = rc;
    return rc;
}
static enum bpf_perf_event_ret receive_perf(void *ctx, int cpu, struct perf_event_header *event)
{
    struct linux_transport *l = ctx;
    int rc;
    if (event->size < sizeof(*event)) rc = -EINVAL;
    else if (event->type == PERF_RECORD_SAMPLE)
        rc = pgwt_transport_perf(l->d->percpu_transport, (uint32_t)cpu, event + 1, event->size - sizeof(*event));
    else if (event->type == PERF_RECORD_LOST && event->size == sizeof(*event) + 16) {
        uint64_t n; memcpy(&n, (unsigned char *)(event + 1) + 8, 8);
        rc = pgwt_transport_lost(l->d->percpu_transport, (uint32_t)cpu, n);
    } else rc = fault(l->d->percpu_transport, PGWT_MERGE_MALFORMED_ENVELOPE);
    if (rc < 0) l->callback_error = rc;
    return LIBBPF_PERF_EVENT_CONT;
}
static int consume_all(struct linux_transport *l, bool *empty)
{
    *empty = true;
    for (size_t i = 0; i < l->ncpu; ++i) {
        uint64_t before = l->d->percpu_transport->positions[i];
        int n = perf_buffer__consume(l->perf[i]);
        if (n < 0) return n;
        /* libbpf consume returns 0 on success, not a record count. Use the
         * reader's existing source position, without another per-DATA
         * increment. libbpf v1.5.0 src/libbpf.c:12812-12828. */
        if (l->d->percpu_transport->positions[i] != before) *empty = false;
    }
    for (uint32_t dest = 1; dest <= 2; ++dest) {
        struct ring_context *r = &l->ring_ctx[dest-1];
        r->consumed = 0; r->limit = 4096;
        int n = ring_buffer__consume(l->rings[dest-1]);
        n = pgwt_transport_ring_result(n, r->consumed, r->limit);
        if (n < 0) return n;
        if (r->consumed) *empty = false;
    }
    return l->callback_error;
}
static int linux_drain(void *ctx)
{
    /* Bounded work: another poll continues the same round if more arrives.
     * Helpers completed snapshot-era attempts. An empty pass through all
     * destinations observes all of those completed publications. */
    bool empty;
    int rc = consume_all(ctx, &empty);
    return rc < 0 ? rc : empty ? 1 : 0;
}
static int linux_helpers_start(void *);
static int linux_configure_io(void *);
static int linux_open_io(void *);
static int linux_epoll_io(void *);
static uint32_t linux_tgid_io(void *);
static int linux_consume_io(void *, uint32_t, bool);
static void linux_close_io(void *);
static int linux_bind(struct pgwt_daemon *d)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf || d->percpu_transport) return 0;
    if (d->mode != PGWT_MODE_FULL || d->lightweight_mode) {
        fprintf(stderr, "exact-perf: requires full watchpoint mode\n"); return -ENOTSUP;
    }
    struct linux_transport *l = calloc(1, sizeof(*l));
    if (!l) return -ENOMEM;
    l->d = d; l->epoll_fd = l->attempts_fd = l->ctl_fd = -1;
    l->possible = libbpf_num_possible_cpus();
    if (l->possible <= 0 || l->possible > 65536) { free(l); return -EINVAL; }
    uint32_t *ids = calloc((size_t)l->possible, sizeof(*ids));
    if (!ids) { free(l); return -ENOMEM; }
    int n = online_cpus(ids, (size_t)l->possible);
    if (n <= 0) { free(ids); free(l); return n; }
    l->ncpu = (size_t)n;
    size_t mask_size = CPU_ALLOC_SIZE((size_t)l->possible);
    cpu_set_t *mask = calloc(1, mask_size);
    bool allowed = mask && !sched_getaffinity(0, mask_size, mask);
    for (size_t i = 0; i < l->ncpu && allowed; ++i) allowed = CPU_ISSET_S(ids[i], mask_size, mask);
    free(mask);
    if (!allowed) { fprintf(stderr, "exact-perf: all online CPUs must be available to pinned helpers\n"); free(ids); free(l); return -ENOTSUP; }
    l->helpers = calloc(l->ncpu, sizeof(*l->helpers)); l->perf = calloc(l->ncpu, sizeof(*l->perf));
    struct pgwt_transport_io io = {.ctx = l, .prepare = linux_helpers_start,
        .configure = linux_configure_io, .open = linux_open_io, .epoll_fd = linux_epoll_io,
        .helper_tgid = linux_tgid_io, .consume = linux_consume_io, .close = linux_close_io, .now = linux_now, .cpu_set_unchanged = linux_topology,
        .control = linux_control, .wake = linux_wake, .attempts = linux_attempts,
        .counters = linux_counters, .drain = linux_drain, .received = linux_received,
        .envelope = linux_envelope, .suspended = linux_suspended, .waiting = linux_waiting,
        .wait = linux_wait, .fault = linux_fault};
    d->percpu_transport = pgwt_transport_create(ids, l->ncpu, 262144, &io);
    free(ids);
    if (!d->percpu_transport || !l->helpers || !l->perf) {
        pgwt_transport_destroy(d->percpu_transport); d->percpu_transport = NULL;
        free(l->helpers); free(l->perf); free(l); return -ENOMEM;
    }
    d->percpu_transport->platform = l;
    return 0;
}
static int linux_helpers_start(void *ctx)
{
    struct linux_transport *l = ctx;
    struct pgwt_daemon *d = l->d;
    size_t mask_size = CPU_ALLOC_SIZE((size_t)l->possible);
    for (size_t i = 0; i < l->ncpu; ++i) {
        struct pinned_helper *h = &l->helpers[i];
        h->cpu = d->percpu_transport->cpus[i]; h->mask_size = mask_size;
        int rc = pthread_mutex_init(&h->mutex, NULL);
        if (rc) { return -rc; }
        rc = pthread_cond_init(&h->cond, NULL);
        if (rc) { pthread_mutex_destroy(&h->mutex); return -rc; }
        h->initialized = true;
        rc = pthread_create(&h->thread, NULL, helper_loop, h);
        if (rc) { return -rc; }
        h->started = true;
        pthread_mutex_lock(&h->mutex);
        while (!h->ready) pthread_cond_wait(&h->cond, &h->mutex);
        rc = h->error;
        pthread_mutex_unlock(&h->mutex);
        if (rc) { return -rc; }
    }
    return 0;
}
static uint32_t linux_tgid_io(void *ctx)
{ return ctx ? (uint32_t)getpid() : 0; }
static struct bpf_program *helper_program(struct pgwt_daemon *d)
{
    struct bpf_program *p, *found = NULL;
    bpf_object__for_each_program(p, d->skel->obj)
        if (!strcmp(bpf_program__section_name(p), "tracepoint/syscalls/sys_enter_getppid")) {
            if (found) return NULL;
            found = p;
        }
    return found;
}
static int linux_configure_io(void *ctx)
{
    struct pgwt_daemon *d = ((struct linux_transport *)ctx)->d;
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    struct linux_transport *l = linux_state(d);
    if (!l || !d->skel) return -EINVAL;
    struct bpf_map *perf = bpf_object__find_map_by_name(d->skel->obj, "perf_events");
    struct bpf_map *ctl = bpf_object__find_map_by_name(d->skel->obj, "transport_ctl");
    struct bpf_map *attempts = bpf_object__find_map_by_name(d->skel->obj, "transport_attempts");
    if (!perf || !ctl || !attempts || !helper_program(d) ||
        bpf_map__type(perf) != BPF_MAP_TYPE_PERF_EVENT_ARRAY ||
        bpf_map__type(ctl) != BPF_MAP_TYPE_ARRAY || bpf_map__value_size(ctl) != 8 ||
        bpf_map__key_size(ctl) != 4 || bpf_map__max_entries(ctl) != 1 ||
        bpf_map__type(attempts) != BPF_MAP_TYPE_PERCPU_ARRAY ||
        bpf_map__key_size(attempts) != 4 || bpf_map__value_size(attempts) != 8 ||
        bpf_map__max_entries(attempts) != 3) {
        fprintf(stderr, "exact-perf: BPF side does not support exact_perf_mode transport (A1 required)\n");
        return -ENOTSUP;
    }
    struct utsname kernel;
    if (uname(&kernel) || strncmp(kernel.release, "4.18.", 5) || !strstr(kernel.release, ".el8")) {
        fprintf(stderr, "exact-perf: production certification requires EL8 4.18; Ubuntu certification is outside this unit\n");
        return -ENOTSUP;
    }
    return bpf_map__set_max_entries(perf, (uint32_t)l->possible);
}
static int add_epoll(int epoll_fd, int fd)
{
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) ? -errno : 0;
}
static int linux_open_io(void *ctx)
{
    struct pgwt_daemon *d = ((struct linux_transport *)ctx)->d;
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    struct linux_transport *l = linux_state(d);
    if (!l || !d->merge_capture) return -EINVAL;
    int perf_fd = bpf_object__find_map_fd_by_name(d->skel->obj, "perf_events");
    l->ctl_fd = bpf_object__find_map_fd_by_name(d->skel->obj, "transport_ctl");
    l->attempts_fd = bpf_object__find_map_fd_by_name(d->skel->obj, "transport_attempts");
    if (perf_fd < 0 || l->ctl_fd < 0 || l->attempts_fd < 0) return -ENOTSUP;
    l->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (l->epoll_fd < 0) return -errno;
    /* One raw perf buffer per online CPU. 64 MiB total, rounded down to a
     * power of two per CPU, at least one page. Match the bench watermark;
     * the daemon's 10 ms poll also drains buffers below that threshold. */
    size_t pages = 1, per_cpu = (64U * 1024U * 1024U) / l->ncpu / (size_t)sysconf(_SC_PAGESIZE);
    while (pages <= per_cpu / 2) pages *= 2;
    for (size_t i = 0; i < l->ncpu; ++i) {
        int cpu = (int)d->percpu_transport->cpus[i];
        struct perf_buffer_raw_opts opts = {.sz = sizeof(opts), .cpu_cnt = 1, .cpus = &cpu, .map_keys = &cpu};
        _Static_assert(PERF_TYPE_SOFTWARE == 1 && PERF_COUNT_SW_BPF_OUTPUT == 10 &&
                       PERF_SAMPLE_RAW == (1U << 10), "portable perf UAPI values");
        struct pgwt_transport_perf_attributes policy = pgwt_transport_perf_attributes(
            pages * (size_t)sysconf(_SC_PAGESIZE));
        struct perf_event_attr attr = {.size = sizeof(attr), .type = policy.type,
            .config = policy.config, .sample_type = policy.sample_type,
            .sample_period = policy.sample_period, .watermark = policy.watermark};
        if (policy.watermark) attr.wakeup_watermark = policy.wakeup_watermark;
        else attr.wakeup_events = policy.wakeup_events;
        l->perf[i] = perf_buffer__new_raw(perf_fd, pages, &attr, receive_perf, l, &opts);
        long err = libbpf_get_error(l->perf[i]);
        if (!l->perf[i] || err) { l->perf[i] = NULL; return err ? (int)err : -errno; }
        int rc = add_epoll(l->epoll_fd, perf_buffer__epoll_fd(l->perf[i]));
        if (rc < 0) return rc;
    }
    const char *names[] = {"event_ringbuf", "lifecycle_rb"};
    for (size_t i = 0; i < 2; ++i) {
        int fd = bpf_object__find_map_fd_by_name(d->skel->obj, names[i]);
        if (fd < 0) return fd;
        l->ring_ctx[i] = (struct ring_context){.d = d, .dest = (uint32_t)i + 1};
        l->rings[i] = ring_buffer__new(fd, receive_ring, &l->ring_ctx[i], NULL);
        long err = libbpf_get_error(l->rings[i]);
        if (!l->rings[i] || err) { l->rings[i] = NULL; return err ? (int)err : -errno; }
        int rc = add_epoll(l->epoll_fd, ring_buffer__epoll_fd(l->rings[i]));
        if (rc < 0) return rc;
    }
    struct pgwt_transport_ctl ctl = {.round = 1};
    if (linux_control(l, &ctl) < 0) return -EIO;
    l->helper_link = bpf_program__attach(helper_program(d));
    long err = libbpf_get_error(l->helper_link);
    if (!l->helper_link || err) { l->helper_link = NULL; return err ? (int)err : -errno; }
    return 0;
}
static int linux_epoll_io(void *ctx)
{ return ((struct linux_transport *)ctx)->epoll_fd; }
static int linux_consume_io(void *ctx, uint32_t dest, bool bounded)
{
    struct pgwt_daemon *d = ((struct linux_transport *)ctx)->d;
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    struct linux_transport *l = linux_state(d);
    if (!l || dest < 1 || dest > 2 || !l->rings[dest-1]) return -EINVAL;
    struct ring_context *r = &l->ring_ctx[dest-1];
    r->consumed = 0; r->limit = bounded ? 4096 : 65536;
    int rc = ring_buffer__consume(l->rings[dest-1]);
    rc = pgwt_transport_ring_result(rc, r->consumed, r->limit);
    return l->callback_error < 0 ? l->callback_error : rc;
}
static void linux_close_io(void *ctx)
{
    struct linux_transport *l = ctx;
    if (!l) return;
    for (size_t i = 0; i < l->ncpu; ++i) if (l->helpers[i].initialized) {
        struct pinned_helper *h = &l->helpers[i];
        pthread_mutex_lock(&h->mutex); h->stop = true;
        pthread_cond_signal(&h->cond); pthread_mutex_unlock(&h->mutex);
    }
    for (size_t i = 0; i < l->ncpu; ++i) {
        struct pinned_helper *h = &l->helpers[i];
        if (h->started) pthread_join(h->thread, NULL);
        if (h->initialized) { pthread_cond_destroy(&h->cond); pthread_mutex_destroy(&h->mutex); }
    }
    if (l->helper_link) bpf_link__destroy(l->helper_link);
    for (size_t i = 0; i < l->ncpu; ++i) if (l->perf[i]) perf_buffer__free(l->perf[i]);
    for (size_t i = 0; i < 2; ++i) if (l->rings[i]) ring_buffer__free(l->rings[i]);
    if (l->epoll_fd >= 0) close(l->epoll_fd);
    free(l->perf); free(l->helpers); free(l);
}

#endif

int pgwt_percpu_transport_prepare(struct pgwt_daemon *d)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    if (!d->percpu_transport) {
#if defined(__linux__) && !defined(PGWT_TRANSPORT_HOST_TEST)
        int rc = linux_bind(d);
        if (rc < 0) return rc;
#else
        fprintf(stderr, "exact-perf: Linux BPF exact_perf_mode support required\n");
        return -ENOTSUP;
#endif
    }
    struct pgwt_percpu_transport *t = d->percpu_transport;
    if (t->prepared) return 0;
    int rc = t->io.prepare ? t->io.prepare(t->io.ctx) : 0;
    if (rc == 0) t->prepared = true;
    else pgwt_percpu_transport_close(d);
    return rc;
}
int pgwt_percpu_transport_configure(struct pgwt_daemon *d)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    struct pgwt_percpu_transport *t = d->percpu_transport;
    if (!t || !t->prepared || !t->io.configure) return -ENOTSUP;
    int rc = t->io.configure(t->io.ctx);
    if (!rc) t->configured = true;
    return rc;
}
size_t pgwt_percpu_transport_cpu_count(const struct pgwt_daemon *d)
{ return d && d->percpu_transport ? d->percpu_transport->ncpu : 0; }
uint32_t pgwt_percpu_transport_cpu_id(const struct pgwt_daemon *d, size_t index)
{
    const struct pgwt_percpu_transport *t = d ? d->percpu_transport : NULL;
    return t && index < t->ncpu ? t->cpus[index] : UINT32_MAX;
}
uint32_t pgwt_percpu_transport_helper_tgid(const struct pgwt_daemon *d)
{
    const struct pgwt_percpu_transport *t = d ? d->percpu_transport : NULL;
    return t && t->prepared && t->io.helper_tgid ? t->io.helper_tgid(t->io.ctx) : 0;
}
int pgwt_percpu_transport_open(struct pgwt_daemon *d)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    struct pgwt_percpu_transport *t = d->percpu_transport;
    if (!t || !t->configured || !t->io.open) return -ENOTSUP;
    if (t->opened) return 0;
    int rc = t->io.open(t->io.ctx);
    if (!rc) t->opened = true;
    return rc;
}
int pgwt_percpu_transport_epoll_fd(const struct pgwt_daemon *d)
{
    const struct pgwt_percpu_transport *t = d ? d->percpu_transport : NULL;
    return t && t->opened && t->io.epoll_fd ? t->io.epoll_fd(t->io.ctx) : -EINVAL;
}
int pgwt_percpu_transport_poll(struct pgwt_daemon *d)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    struct pgwt_percpu_transport *t = d->percpu_transport;
    if (!t || !t->opened) return -EINVAL;
    int rc = t->io.drain(t->io.ctx);
    if (rc < 0) return rc;
    uint64_t now = observed(t);
    if (!pgwt_transport_service_due(now, t->last_service_at, t->serviced))
        return complete_round(t);
    t->last_service_at = now; t->serviced = true;
    return pgwt_transport_poll(t, pgwt_capture_waiting(d->merge_capture));
}
int pgwt_percpu_transport_snapshot(struct pgwt_daemon *d, ma_snapshot *s, uint64_t *now)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    if (!s || !now || !d->percpu_transport) return -EINVAL;
    struct pgwt_percpu_transport *t = d->percpu_transport;
    counters(t);
    *s = t->snapshot;
    *now = observed(t);
    return 0;
}
int pgwt_percpu_transport_consume(struct pgwt_daemon *d, uint32_t dest, bool bounded)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    struct pgwt_percpu_transport *t = d->percpu_transport;
    return t && t->opened && t->io.consume ? t->io.consume(t->io.ctx, dest, bounded) : -EINVAL;
}
int pgwt_percpu_transport_record(struct pgwt_daemon *d, uint32_t dest, const void *p, size_t n)
{
    if (!d) return -EINVAL;
    if (!d->exact_perf) return 0;
    return pgwt_transport_record(d->percpu_transport, dest, p, n);
}
void pgwt_percpu_transport_close(struct pgwt_daemon *d)
{
    if (!d || !d->percpu_transport) return;
    struct pgwt_percpu_transport *t = d->percpu_transport;
    if (t->io.close) t->io.close(t->io.ctx);
    pgwt_transport_destroy(t); d->percpu_transport = NULL;
}
