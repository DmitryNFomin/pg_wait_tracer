#define _GNU_SOURCE
#include "rt_priority.h"

#include <dirent.h>
#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

static pid_t rt_tid(void) { return (pid_t)syscall(SYS_gettid); }

static int rt_threads(int (*visit)(pid_t, void *), void *arg)
{
    DIR *dir = opendir("/proc/self/task");
    if (!dir) return -1;
    struct dirent *entry;
    int result = 0, error = 0;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) { if (errno) { result = -1; error = errno; } break; }
        char *end;
        long tid = strtol(entry->d_name, &end, 10);
        if (*end || tid <= 0) continue;
        if (visit((pid_t)tid, arg) < 0) { result = -1; error = errno; break; }
    }
    closedir(dir);
    errno = error;
    return result;
}

static int rt_get(pid_t tid, int *policy, int *priority)
{
    struct sched_param param;
    *policy = sched_getscheduler(tid);
    if (*policy < 0 || sched_getparam(tid, &param) < 0) return -1;
    *priority = param.sched_priority;
    return 0;
}

static int rt_set(pid_t tid, int policy, int priority)
{
    struct sched_param param = { .sched_priority = priority };
    /* Direct syscall is also used by the asynchronous watchdog handler. */
    return (int)syscall(SYS_sched_setscheduler, tid, policy, &param);
}

static int rt_limits(unsigned long soft, unsigned long hard)
{
    struct rlimit limit = { .rlim_cur = soft, .rlim_max = hard }, actual;
    if (setrlimit(RLIMIT_RTTIME, &limit) < 0 ||
        getrlimit(RLIMIT_RTTIME, &actual) < 0) return -1;
    if (actual.rlim_cur != soft || actual.rlim_max != hard) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int rt_handler(void (*fn)(int))
{
    struct sigaction action = { .sa_handler = fn, .sa_flags = SA_RESTART };
    sigemptyset(&action.sa_mask);
    return sigaction(SIGXCPU, &action, NULL);
}

static int rt_mask(bool block)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGXCPU);
    return sigprocmask(block ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL);
}

static void rt_warning(const char *msg, size_t size)
{
    /* No allocation, locks, or stdio: callable after handler demotion. */
    while (size) {
        ssize_t n = write(STDERR_FILENO, msg, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        msg += n;
        size -= (size_t)n;
    }
}

const struct pgwt_rt_ops pgwt_rt_linux_ops = {
    rt_tid, rt_threads, rt_get, rt_set, rt_limits, rt_handler, rt_mask, rt_warning
};
