/* ctrl_outq.h — per-client output queue for the control socket (#424).
 *
 * WHY THIS EXISTS. The control socket's client fds are NONBLOCKING, and the
 * old sender looped on send() and treated EAGAIN as "the client stopped
 * reading" — drop it. That is sound for a 2 KB status reply and WRONG for the
 * open-intervals snapshot: a reply of hundreds of KB does not fit one socket
 * buffer, so EAGAIN is the NORMAL case, and dropping there produces a
 * truncated JSON line, which is the one outcome the #424 contract forbids
 * (a partial answer must surface as a degraded status, never as data).
 *
 * A bigger buffer does not fix it: SO_SNDBUF is finite whatever the reply
 * size. What fixes it is queueing the remainder and finishing the write when
 * the socket says it can take more (EPOLLOUT) — backpressure, not a bet on
 * buffer sizes.
 *
 * The queue is pure: it writes through a caller-supplied function, so the unit
 * test can inject partial writes, EAGAIN storms and hard errors without a
 * socket. */
#ifndef PGWT_CTRL_OUTQ_H
#define PGWT_CTRL_OUTQ_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* Hard cap on one client's unsent backlog. Two full-size snapshot replies: a
 * client that has stopped reading entirely must cost the daemon a bounded
 * amount of memory and then be dropped, never grow without limit. */
#define PGWT_CTRL_OUTQ_MAX  (2u * 1024u * 1024u)

struct pgwt_ctrl_outq {
    char    *buf;
    size_t   cap;     /* allocated bytes */
    size_t   len;     /* bytes in buf */
    size_t   off;     /* bytes already written out of [0, len) */
    bool     overflow;/* the cap was hit: this client is unrecoverable */
};

/* Write callback: must return the number of bytes written (> 0), or -1 with
 * errno set (EAGAIN/EWOULDBLOCK = "would block", anything else = fatal). */
typedef ssize_t (*pgwt_ctrl_write_fn)(void *ctx, const char *p, size_t n);

static inline void pgwt_outq_init(struct pgwt_ctrl_outq *q)
{
    q->buf = NULL;
    q->cap = q->len = q->off = 0;
    q->overflow = false;
}

static inline void pgwt_outq_reset(struct pgwt_ctrl_outq *q)
{
    free(q->buf);
    pgwt_outq_init(q);
}

static inline size_t pgwt_outq_pending(const struct pgwt_ctrl_outq *q)
{
    return q->len - q->off;
}

/* Queue n bytes. Returns 0, or -1 when the backlog cap would be exceeded (the
 * queue is then marked overflow and the caller must drop the client — it can
 * no longer be sent a correct reply, and sending a shortened one would be a
 * partial answer). */
static inline int pgwt_outq_append(struct pgwt_ctrl_outq *q,
                                   const char *p, size_t n)
{
    if (q->overflow)
        return -1;
    /* Compact first: a fully-drained queue costs nothing to reuse. */
    if (q->off > 0 && q->off == q->len)
        q->len = q->off = 0;
    else if (q->off > 0) {
        memmove(q->buf, q->buf + q->off, q->len - q->off);
        q->len -= q->off;
        q->off = 0;
    }
    if (q->len + n > PGWT_CTRL_OUTQ_MAX) {
        q->overflow = true;
        return -1;
    }
    if (q->len + n > q->cap) {
        size_t want = q->cap ? q->cap : 4096;
        while (want < q->len + n)
            want *= 2;
        char *nb = (char *)realloc(q->buf, want);
        if (!nb) {
            q->overflow = true;
            return -1;
        }
        q->buf = nb;
        q->cap = want;
    }
    memcpy(q->buf + q->len, p, n);
    q->len += n;
    return 0;
}

/* Push as much as the socket takes.
 *   1  = queue fully drained (stop watching for writability)
 *   0  = bytes remain (watch for writability and call again)
 *  -1  = fatal write error (drop the client)
 * A zero-byte write counts as "would block" rather than looping forever. */
static inline int pgwt_outq_flush(struct pgwt_ctrl_outq *q,
                                  pgwt_ctrl_write_fn fn, void *ctx)
{
    while (q->off < q->len) {
        ssize_t w = fn(ctx, q->buf + q->off, q->len - q->off);
        if (w > 0) {
            q->off += (size_t)w;
            continue;
        }
        if (w == 0)
            return 0;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    q->len = q->off = 0;
    /* Keep the allocation for the next reply unless it is large: one idle
     * client must not pin a megabyte. */
    if (q->cap > 64u * 1024u) {
        free(q->buf);
        q->buf = NULL;
        q->cap = 0;
    }
    return 1;
}

#endif /* PGWT_CTRL_OUTQ_H */
