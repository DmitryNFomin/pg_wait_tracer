/* control.h — Daemon control socket: unix socket + JSON-line protocol
 *
 * Listens on {trace_dir}/pgwt.sock (mode 0600). One JSON object per
 * line, request/response. Commands (Phase A0):
 *   {"cmd":"status"}  → mode, uptime, backends tracked, pg_pid, version
 *     Status also carries exact_probe_{exec,plan}_attach_outcome
 *     (not_attempted | disabled | attached | attach_failed). Status and metrics
 *     carry *_attach_errno beside exact_probe_attached_mask; outcome strings
 *     are status-only to keep metrics numeric. Outcomes describe the latest
 *     attempt or explicit policy refusal (lightweight, skip-USDT, or COOP),
 *     retained after release; the mask describes current links. errno 0 on a
 *     failure means unknown. ENOTSUP is an attach error, not a support claim:
 *     kernel/binary support is not measured independently here. These flat
 *     diagnostics follow the cpu_clamped/offcpu disclosure idiom and do not
 *     change window_fidelity or fidelity_ranges.
 *   {"cmd":"metrics"} → self-observability counters (stable snake_case
 *                       names with units: *_total, *_per_sec, *_bytes —
 *                       a future Prometheus exporter consumes these)
 *   {"cmd":"open_intervals"} → every live backend's still-OPEN wait/on-CPU
 *                       stretch, with the clock anchor and the publication
 *                       watermark needed to reconcile them against the
 *                       trace (#424). Shape and degraded contract:
 *                       src/open_snapshot.h. Replies here are far larger
 *                       than one socket buffer, which is why responses are
 *                       QUEUED (src/ctrl_outq.h) instead of dropped on
 *                       EAGAIN.
 *
 * Note on ringbuf drops: the BPF ringbuf does not expose a drop
 * counter to userspace, and A0 makes no BPF changes. A2 (provider
 * split) should add a BPF-side drop counter map; until then drop
 * counts are not reported rather than reported as a misleading 0.
 */
#ifndef PGWT_CONTROL_H
#define PGWT_CONTROL_H

#include "ctrl_outq.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pgwt_daemon;

#define PGWT_CTRL_MAX_CLIENTS 16
#define PGWT_CTRL_BUF_SIZE    4096

/* Per-client connection state (line-buffered request parsing) */
struct pgwt_control_client {
    int    fd;                       /* -1 = free slot */
    size_t len;                      /* bytes buffered (no '\n' yet) */
    char   buf[PGWT_CTRL_BUF_SIZE];
    /* #424: unsent response bytes. The open-intervals snapshot is far larger
     * than one socket buffer, so EAGAIN is normal and must mean "finish this
     * later" (EPOLLOUT) rather than "drop the client" — dropping mid-reply
     * produces truncated JSON, the one failure mode the contract forbids. */
    struct pgwt_ctrl_outq out;
    bool   want_write;               /* EPOLLOUT is currently registered */
};

struct pgwt_control {
    int    listen_fd;
    int    epoll_fd;                 /* daemon's epoll (not owned) */
    char   sock_path[520];
    struct pgwt_control_client clients[PGWT_CTRL_MAX_CLIENTS];
    struct pgwt_daemon *d;           /* back-pointer for status/metrics */
};

/* Create {trace_dir}/pgwt.sock (mode 0600, stale socket unlinked) and
 * register the listening fd with epoll_fd. Returns 0 on success. */
int pgwt_control_init(struct pgwt_control *c, struct pgwt_daemon *d,
                      int epoll_fd);

/* Dispatch an epoll-ready fd. Returns 1 if the fd belongs to the
 * control socket (listener or client) and was handled, 0 otherwise. */
int pgwt_control_handle_fd(struct pgwt_control *c, int fd);

/* Close all clients and the listener, unlink the socket. */
void pgwt_control_cleanup(struct pgwt_control *c);

#endif /* PGWT_CONTROL_H */
