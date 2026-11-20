#define _GNU_SOURCE
#include "rt_priority.h"
#include "thread_other.h"
#include <assert.h>
#include <linux/capability.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct worker { pthread_t thread; int ready[2], release[2]; };
static void *wait_worker(void *arg)
{
    struct worker *w = arg;
    sigset_t mask;
    sigemptyset(&mask); sigaddset(&mask, SIGXCPU);
    assert(pthread_sigmask(SIG_BLOCK, &mask, NULL) == 0);
    assert(write(w->ready[1], "r", 1) == 1);
    char byte;
    assert(read(w->release[0], &byte, 1) == 1);
    return NULL;
}

static void create(struct worker *w, bool explicit_policy)
{
    assert(pipe(w->ready) == 0 && pipe(w->release) == 0);
    assert((explicit_policy ? pgwt_thread_create_other(&w->thread, wait_worker, w)
                            : pthread_create(&w->thread, NULL, wait_worker, w)) == 0);
    char byte;
    assert(read(w->ready[0], &byte, 1) == 1);
}

static int check(pid_t tid, void *arg)
{
    struct pgwt_rt_priority *rt = arg;
    int policy, priority;
    assert(rt->ops->get(tid, &policy, &priority) == 0);
    bool active = tid == rt->main_tid && rt->state == PGWT_RT_ACTIVE;
    assert((policy & ~PGWT_RT_RESET) == (active ? PGWT_RT_FIFO : PGWT_RT_OTHER));
    assert(priority == (active ? 1 : 0));
    printf("probe tid=%d policy=%d priority=%d\n", tid, policy & ~PGWT_RT_RESET, priority);
    return 0;
}

int main(void)
{
    struct pgwt_rt_priority rt;
    struct worker workers[3];
    pgwt_rt_init(&rt, &pgwt_rt_linux_ops);
    /* The live runner launches this under FIFO 7, including this worker. */
    create(&workers[0], false);
    pgwt_rt_prepare(&rt);
    rt.soft_us = 20000; rt.hard_us = 2000000;
    pgwt_rt_start(&rt);
    assert(rt.state == PGWT_RT_ACTIVE);
    /* Prove the kernel's reset flag works even with default pthread attrs,
     * and prove the production explicit-policy wrapper after activation. */
    create(&workers[1], false);
    create(&workers[2], true);
    assert(rt.ops->threads(check, &rt) == 0);
    struct __user_cap_header_struct header = { .version = _LINUX_CAPABILITY_VERSION_3 };
    struct __user_cap_data_struct caps[2];
    assert(syscall(SYS_capget, &header, caps) == 0);
    caps[CAP_SYS_NICE / 32].effective &= ~(1U << (CAP_SYS_NICE % 32));
    caps[CAP_SYS_NICE / 32].permitted &= ~(1U << (CAP_SYS_NICE % 32));
    assert(syscall(SYS_capset, &header, caps) == 0);
    struct timespec start, end;
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start) == 0);
    while (rt.state == PGWT_RT_ACTIVE) __asm__ volatile("" ::: "memory");
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end) == 0);
    assert(rt.state == PGWT_RT_DEMOTED && rt.demotions == 1 && rt.demote_errno == 0);
    assert(rt.ops->threads(check, &rt) == 0);
    printf("probe kernel watchdog without CAP_SYS_NICE: cpu_us=%lld soft_us=%lu PASS\n",
           (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_nsec - start.tv_nsec) / 1000,
           rt.soft_us);
    for (int i = 0; i < 3; ++i) {
        assert(write(workers[i].release[1], "x", 1) == 1);
        assert(pthread_join(workers[i].thread, NULL) == 0);
        close(workers[i].ready[0]); close(workers[i].ready[1]);
        close(workers[i].release[0]); close(workers[i].release[1]);
    }
    return 0;
}
