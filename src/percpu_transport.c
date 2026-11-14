/* U1b-0 seams only. B supplies the reader and completion protocol. */
#include "percpu_transport.h"
#include <errno.h>
#include <stdio.h>

int pgwt_percpu_transport_prepare(struct pgwt_daemon *d)
{
    (void)d;
    fprintf(stderr, "FATAL: exact per-CPU transport is not built yet\n");
    return -ENOSYS;
}

int pgwt_percpu_transport_open(struct pgwt_daemon *d)
{
    (void)d;
    return -ENOSYS;
}

int pgwt_percpu_transport_configure(struct pgwt_daemon *d)
{
    (void)d;
    return -ENOSYS;
}

uint32_t pgwt_percpu_transport_helper_tgid(const struct pgwt_daemon *d)
{
    (void)d;
    return 0; /* no helper exists in the precursor */
}

int pgwt_percpu_transport_epoll_fd(const struct pgwt_daemon *d)
{
    (void)d;
    return -ENOSYS;
}

int pgwt_percpu_transport_poll(struct pgwt_daemon *d)
{
    (void)d;
    return -ENOSYS;
}

int pgwt_percpu_transport_consume(struct pgwt_daemon *d, uint32_t dest,
                                 bool bounded)
{
    (void)d; (void)dest; (void)bounded;
    return -ENOSYS;
}

int pgwt_percpu_transport_record(struct pgwt_daemon *d, uint32_t dest,
                                const void *bytes, size_t size)
{
    (void)d; (void)dest; (void)bytes; (void)size;
    return -ENOSYS;
}

void pgwt_percpu_transport_close(struct pgwt_daemon *d)
{
    (void)d;
}
