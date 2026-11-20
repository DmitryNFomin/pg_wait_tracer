#include "rt_priority.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Only main unmasks SIGXCPU. Workers inherit it blocked from prepare(). */
static struct pgwt_rt_priority *watched;

static int normal_thread(pid_t tid, void *arg)
{
    struct pgwt_rt_priority *rt = arg;
    int policy, priority;
    if (rt->ops->get(tid, &policy, &priority) < 0)
        return errno == ESRCH ? 0 : -1;
    if ((policy & ~PGWT_RT_RESET) != PGWT_RT_OTHER || priority != 0) {
        /* Keep RESET even when demoting: clearing it requires CAP_SYS_NICE. */
        if (rt->ops->set(tid, PGWT_RT_OTHER | PGWT_RT_RESET, 0) < 0)
            return errno == ESRCH ? 0 : -1;
    }
    return 0;
}

static int verify_thread(pid_t tid, void *arg)
{
    struct pgwt_rt_priority *rt = arg;
    int policy, priority;
    if (rt->ops->get(tid, &policy, &priority) < 0)
        return errno == ESRCH && tid != rt->main_tid ? 0 : -1;
    bool is_main = tid == rt->main_tid && rt->state == PGWT_RT_ACTIVE;
    if ((policy & ~PGWT_RT_RESET) != (is_main ? PGWT_RT_FIFO : PGWT_RT_OTHER) ||
        priority != (is_main ? 1 : 0) || (is_main && !(policy & PGWT_RT_RESET))) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static void refuse(struct pgwt_rt_priority *rt, const char *operation, int error)
{
    /* Demote main without depending on /proc enumeration or readback, which
     * may be precisely the failed operation (for example EMFILE). */
    int main_rollback = rt->ops->set(rt->main_tid, PGWT_RT_OTHER | PGWT_RT_RESET, 0);
    int rollback = rt->ops->threads(normal_thread, rt);
    if (main_rollback < 0) rollback = -1;
    rt->state = PGWT_RT_REFUSED;
    if (!rt->warned) {
        char msg[768];
        int n = snprintf(msg, sizeof(msg),
            "WARN: rt_priority refused: %s: %s. Continuing at normal priority; "
            "short-lived processes may be missed under load. Grant CAP_SYS_NICE "
            "(systemd: AmbientCapabilities=CAP_SYS_NICE); on EL8 with "
            "CONFIG_RT_GROUP_SCHED, use a service/scope without CPUQuota and "
            "with an RT runtime allowance. Check LimitRTPRIO/LimitRTTIME. %s\n",
            operation, strerror(error), rollback < 0
            ? "WARNING: scheduler rollback failed; inherited policy could remain."
            : "");
        rt->ops->warning(msg, (size_t)n);
        rt->warned = true;
    }
}

static void cpu_limit(int signo)
{
    (void)signo;
    int saved = errno;
    struct pgwt_rt_priority *rt = watched;
    if (rt && rt->state == PGWT_RT_ACTIVE && !rt->demotions) {
        if (rt->ops->set(rt->main_tid, PGWT_RT_OTHER | PGWT_RT_RESET, 0) == 0) {
            rt->demotions = 1;
            rt->state = PGWT_RT_DEMOTED;
            /* Demote BEFORE writing: a full diagnostic pipe can block, but
             * can no longer hold a CPU at FIFO priority. No stdio in handler. */
            rt->ops->warning(rt->demotion_warning, rt->demotion_warning_len);
        } else {
            rt->demote_errno = errno;
            /* Do not claim success. The kernel's hard RLIMIT_RTTIME remains
             * armed and kills a thread which cannot be demoted. */
        }
    }
    errno = saved;
}

void pgwt_rt_init(struct pgwt_rt_priority *rt, const struct pgwt_rt_ops *ops)
{
    memset(rt, 0, sizeof(*rt));
    rt->ops = ops;
    rt->soft_us = PGWT_RT_SOFT_US;
    rt->hard_us = PGWT_RT_HARD_US;
}

void pgwt_rt_prepare(struct pgwt_rt_priority *rt)
{
    rt->main_tid = rt->ops->tid();
    /* Reset inherited FIFO before any pthread or subprocess can inherit it.
     * Keep SIGXCPU blocked in every future worker; only main unblocks it. */
    if (rt->ops->mask(true) < 0 ||
        rt->ops->set(rt->main_tid, PGWT_RT_OTHER | PGWT_RT_RESET, 0) < 0 ||
        rt->ops->threads(normal_thread, rt) < 0) {
        int error = errno;
        rt->attempted = true;
        refuse(rt, "normalizing inherited scheduler", error);
        return;
    }
    rt->prepared = true;
}

void pgwt_rt_start(struct pgwt_rt_priority *rt)
{
    if (!rt->prepared || rt->state == PGWT_RT_REFUSED ||
        rt->state == PGWT_RT_DEMOTED)
        return;
    if (rt->disabled) {
        rt->state = PGWT_RT_DISABLED;
        if (rt->ops->threads(normal_thread, rt) < 0 ||
            rt->ops->threads(verify_thread, rt) < 0)
            refuse(rt, "verifying disabled scheduler", errno);
        return;
    }
    if (!rt->attempted) {
        rt->attempted = true;
        if (!rt->soft_us || rt->hard_us <= rt->soft_us || rt->hard_us == ULONG_MAX) {
            refuse(rt, "watchdog limits (require finite 0 < soft < hard)", EINVAL);
            return;
        }
        rt->demotion_warning_len = (size_t)snprintf(
            rt->demotion_warning, sizeof(rt->demotion_warning),
            "WARN: rt_priority demoted: main loop reached the %lu us "
            "real-time CPU watchdog limit without blocking (RLIMIT_RTTIME; "
            "kernel tick accounting). Continuing at normal priority for "
            "the rest of this process.\n", rt->soft_us);
        watched = rt;
        if (rt->ops->handler(cpu_limit) < 0 ||
            rt->ops->limits(rt->soft_us, rt->hard_us) < 0) {
            refuse(rt, "arming RLIMIT_RTTIME/SIGXCPU watchdog", errno);
            return;
        }
    }
    if (rt->ops->mask(true) < 0 ||
        rt->ops->threads(normal_thread, rt) < 0 ||
        rt->ops->set(rt->main_tid, PGWT_RT_FIFO | PGWT_RT_RESET, 1) < 0) {
        refuse(rt, "SCHED_FIFO priority 1", errno);
        return;
    }
    rt->state = PGWT_RT_ACTIVE;
    if (rt->ops->threads(verify_thread, rt) < 0 || rt->ops->mask(false) < 0)
        refuse(rt, "verifying main/worker policies or unblocking SIGXCPU", errno);
}

void pgwt_rt_pause(struct pgwt_rt_priority *rt)
{
    if (rt->prepared &&
        rt->ops->set(rt->main_tid, PGWT_RT_OTHER | PGWT_RT_RESET, 0) < 0)
        refuse(rt, "leaving main loop", errno);
}

const char *pgwt_rt_state_name(const struct pgwt_rt_priority *rt)
{
    switch (rt->state) {
    case PGWT_RT_ACTIVE: return "active";
    case PGWT_RT_REFUSED: return "refused";
    case PGWT_RT_DEMOTED: return "demoted";
    default: return "disabled";
    }
}
