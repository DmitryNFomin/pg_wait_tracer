#ifndef PGWT_PERCPU_TRANSPORT_H
#define PGWT_PERCPU_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pgwt_transport_wire.h"

struct pgwt_daemon;
struct pgwt_percpu_transport;

/* Callback dispatch keeps existing isolated lifecycle tests linkable without
 * transport I/O. Daemon init installs the production functions below. */
struct pgwt_transport_callbacks {
    int (*consume)(struct pgwt_daemon *, uint32_t dest, bool bounded);
    int (*record)(struct pgwt_daemon *, uint32_t dest,
                  const void *bytes, size_t size);
};

/* B owns these seams. Negative errno on failure, never silently succeed.
 * PGWT_TRANSPORT_HOST_TEST selects portable test I/O in scripts/check.sh;
 * B keeps Linux loader/reader dependencies out of that compilation.
 * prepare is idempotent (CLI and daemon init both call it), runs before
 * discovery/loading, and cleans up its own partial failure. open runs after
 * skeleton load and before producer attachment.
 * The nested epoll fd covers CPU, E and L; poll must be bounded and service
 * completion rounds/held data even when no fd was ready. */
int pgwt_percpu_transport_prepare(struct pgwt_daemon *d);
/* Called after skeleton open, before load: map sizing, helper identity and
 * rodata setup owned by B. helper_tgid is available after prepare(). */
int pgwt_percpu_transport_configure(struct pgwt_daemon *d);
uint32_t pgwt_percpu_transport_helper_tgid(const struct pgwt_daemon *d);
size_t pgwt_percpu_transport_cpu_count(const struct pgwt_daemon *);
uint32_t pgwt_percpu_transport_cpu_id(const struct pgwt_daemon *, size_t);
int pgwt_percpu_transport_open(struct pgwt_daemon *d);
int pgwt_percpu_transport_epoll_fd(const struct pgwt_daemon *d);
int pgwt_percpu_transport_poll(struct pgwt_daemon *d);
/* consume returns records consumed, or negative errno. bounded is the
 * existing event-loop budget request; callers must not drain raw rings. */
int pgwt_percpu_transport_consume(struct pgwt_daemon *d, uint32_t dest,
                                 bool bounded);
int pgwt_percpu_transport_record(struct pgwt_daemon *d, uint32_t dest,
                                const void *bytes, size_t size);
void pgwt_percpu_transport_close(struct pgwt_daemon *d);

/* Portable reader/protocol. CPU IDs are sorted physical online CPU numbers;
 * source indices are their ordinal positions followed by E and L. All I/O,
 * including completion-delimited draining, is supplied by this interface. */
#include "core/pgwt_merge_adapter.h"
struct pgwt_transport_io {
    void *ctx;
    /* Optional daemon binding operations. Injected readers can replace the
     * entire kernel lifetime, including map/support checks and helper setup. */
    int (*prepare)(void *);
    int (*configure)(void *);
    int (*open)(void *);
    int (*epoll_fd)(void *);
    uint32_t (*helper_tgid)(void *);
    int (*consume)(void *, uint32_t dest, bool bounded);
    void (*close)(void *);
    uint64_t (*now)(void *);
    int (*cpu_set_unchanged)(void *);
    int (*control)(void *, const struct pgwt_transport_ctl *);
    int (*wake)(void *, uint32_t cpu);
    /* cpu-major triples, CPU/E/L, across exactly the participating CPUs. */
    int (*attempts)(void *, uint64_t *, size_t);
    int (*counters)(void *, ma_snapshot *);
    /* Drain each destination until empty after acknowledgements; BUSY or a
     * bounded drain returns 0, complete returns 1, negative errno on error. */
    int (*drain)(void *);
    int (*received)(void *, const ma_envelope *, uint64_t);
    /* Return the latest post-publication snapshot through the last argument;
     * a later queued envelope must never replay a pre-callback observation. */
    int (*envelope)(void *, const ma_envelope *, uint64_t, ma_snapshot *);
    bool (*suspended)(void *);
    int (*fault)(void *, pgwt_merge_reason, uint64_t, const ma_snapshot *);
};
struct pgwt_wire_record {
    ma_envelope envelope;
    uint32_t cpu, dest, round;
    uint64_t seq;
};
/* Decode only: position is supplied by the consuming reader. Payload is
 * borrowed; all fields are read explicitly as little endian, unaligned. */
int pgwt_transport_decode(const uint32_t *cpus, size_t ncpu, uint32_t dest,
                          uint64_t position, const void *, size_t,
                          struct pgwt_wire_record *);
struct pgwt_percpu_transport *pgwt_transport_create(
    const uint32_t *cpus, size_t ncpu, size_t capacity,
    const struct pgwt_transport_io *);
int pgwt_transport_record(struct pgwt_percpu_transport *, uint32_t dest,
                           const void *, size_t);
/* perf RAW framing includes u32 advertised size and kernel alignment padding. */
int pgwt_transport_perf(struct pgwt_percpu_transport *, uint32_t cpu,
                        const void *, size_t);
int pgwt_transport_lost(struct pgwt_percpu_transport *, uint32_t cpu, uint64_t);
int pgwt_transport_poll(struct pgwt_percpu_transport *, bool waiting);
int pgwt_transport_finish(struct pgwt_percpu_transport *);
void pgwt_transport_destroy(struct pgwt_percpu_transport *);
bool pgwt_transport_lost_total(const struct pgwt_percpu_transport *, uint64_t *);
uint64_t pgwt_transport_max_held_ns(const struct pgwt_percpu_transport *);

#endif
