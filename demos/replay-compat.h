/* Compatibility for the demo's local offline replay build only.
 * Linux capture builds keep the native APIs. The replay server is single
 * threaded: pipe/socket + fcntl preserves close-on-exec without a race here.
 */
#ifndef PGWT_DEMO_REPLAY_COMPAT_H
#define PGWT_DEMO_REPLAY_COMPAT_H
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
/* The README's replay must show capture-machine capacity, not laptop capacity.
 * Only the demo build honors this environment variable. Other sysconf queries
 * retain the platform implementation (e.g. the reader's page-size query).
 */
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
static inline long pgwt_demo_sysconf(int name)
{
    const char *value = getenv("PGWT_DEMO_CPUS");
    if (name == _SC_NPROCESSORS_ONLN && value && *value) {
        char *end;
        long count = strtol(value, &end, 10);
        if (!*end && count > 0 && count <= INT_MAX) return count;
    }
    return sysconf(name);
}
#define sysconf pgwt_demo_sysconf

#ifdef __APPLE__
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define st_mtim st_mtimespec
#define SOCK_CLOEXEC (1 << 30)

static inline int pgwt_demo_socket(int domain, int type, int protocol)
{
    int fd = socket(domain, type & ~SOCK_CLOEXEC, protocol);
    if (fd >= 0 && (type & SOCK_CLOEXEC) &&
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static inline int pgwt_demo_pipe2(int fds[2], int flags)
{
    if (flags & ~O_CLOEXEC) {
        errno = EINVAL;
        return -1;
    }
    if (pipe(fds) < 0) return -1;
    if ((flags & O_CLOEXEC) &&
        (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 ||
         fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0)) {
        int saved = errno;
        close(fds[0]);
        close(fds[1]);
        errno = saved;
        return -1;
    }
    return 0;
}

#define socket pgwt_demo_socket
#define pipe2 pgwt_demo_pipe2
#endif
#endif
