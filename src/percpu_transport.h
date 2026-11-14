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

#endif
