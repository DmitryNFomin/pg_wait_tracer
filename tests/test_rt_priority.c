/* Run the production policy/watchdog with injected syscalls, including Darwin. */
#include "rt_priority.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, name) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", name); return 1; } } while (0)
static int policy[4], priority[4], total, refused, warnings, set_calls;
static int fail_limits, fail_handler, fail_mask, bad_readback, fail_demotion;
static bool blocked, fail_enumeration;
static unsigned long soft_limit, hard_limit;
static void (*signal_handler)(int);
static char message[4096];
static pid_t tid(void) { return 1; }
static int threads(int (*visit)(pid_t, void *), void *arg)
{
    if (fail_enumeration && (policy[1] & ~PGWT_RT_RESET) == PGWT_RT_FIFO) {
        errno = EMFILE;
        return -1;
    }
    for (int i = 1; i <= total; ++i)
        if (visit(i, arg) < 0) return -1;
    return 0;
}
static int get(pid_t id, int *p, int *v)
{
    *p = policy[id]; *v = priority[id];
    if (bad_readback && id == 2 && (policy[1] & ~PGWT_RT_RESET) == PGWT_RT_FIFO)
        *p = PGWT_RT_FIFO;
    return 0;
}
static int set(pid_t id, int p, int v)
{
    ++set_calls;
    if ((refused && (p & ~PGWT_RT_RESET) == PGWT_RT_FIFO) ||
        (fail_demotion && (p & ~PGWT_RT_RESET) == PGWT_RT_OTHER)) {
        errno = EPERM; return -1;
    }
    /* Model the kernel's privilege check when clearing RESET. */
    if ((policy[id] & PGWT_RT_RESET) && !(p & PGWT_RT_RESET)) {
        errno = EPERM; return -1;
    }
    policy[id] = p; priority[id] = v;
    return 0;
}
static int limits(unsigned long soft, unsigned long hard)
{
    soft_limit = soft; hard_limit = hard;
    if (fail_limits) { errno = EPERM; return -1; }
    return 0;
}
static int handler(void (*fn)(int))
{
    signal_handler = fn;
    if (fail_handler) { errno = EINVAL; return -1; }
    return 0;
}
static int mask(bool block)
{
    blocked = block;
    if (fail_mask && !block) { errno = EINVAL; return -1; }
    return 0;
}
static void warning(const char *s, size_t n)
{
    ++warnings;
    if (n >= sizeof(message)) abort();
    memcpy(message, s, n); message[n] = 0;
}
static const struct pgwt_rt_ops ops = {tid, threads, get, set, limits, handler, mask, warning};

int main(int argc, char **argv)
{
    const char *arm = argc > 1 ? argv[1] : "success";
    printf("REACHED: %s\n", arm); fflush(stdout);
    struct pgwt_rt_priority rt;
    pgwt_rt_init(&rt, &ops);
    total = 2;
    if (!strcmp(arm, "inherited") || !strcmp(arm, "disabled")) {
        policy[1] = policy[2] = PGWT_RT_FIFO;
        priority[1] = priority[2] = 7;
    }
    pgwt_rt_prepare(&rt);
    CHECK(blocked, "workers inherit blocked SIGXCPU");
    CHECK((policy[1] & ~PGWT_RT_RESET) == PGWT_RT_OTHER &&
          (policy[2] & ~PGWT_RT_RESET) == PGWT_RT_OTHER, "inherited FIFO normalized before workers");
    if (!strcmp(arm, "refused")) refused = 1;
    if (!strcmp(arm, "disabled")) rt.disabled = true;
    if (!strcmp(arm, "limits")) fail_limits = 1;
    if (!strcmp(arm, "handler")) fail_handler = 1;
    if (!strcmp(arm, "mask")) fail_mask = 1;
    if (!strcmp(arm, "readback")) bad_readback = 1;
    if (!strcmp(arm, "enumeration")) fail_enumeration = true;
    rt.soft_us = 123000; rt.hard_us = 456000;
    pgwt_rt_start(&rt);
    if (refused || fail_limits || fail_handler || fail_mask || bad_readback || fail_enumeration) {
        CHECK(!strcmp(pgwt_rt_state_name(&rt), "refused"), "refusal state recorded");
        CHECK((policy[1] & ~PGWT_RT_RESET) == PGWT_RT_OTHER &&
              (policy[2] & ~PGWT_RT_RESET) == PGWT_RT_OTHER, "refusal normal scheduler rollback");
        CHECK(warnings == 1 && strstr(message, "WARN:") &&
              strstr(message, "short-lived processes may be missed under load") &&
              strstr(message, "CAP_SYS_NICE") && strstr(message, "CPUQuota") &&
              strstr(message, refused ? strerror(EPERM) : "refused"), "refusal actionable warning exactly once");
        pgwt_rt_start(&rt);
        CHECK(warnings == 1, "refusal warning stays one across reconnect");
        puts("CONTINUES: capture loop after refusal");
    } else if (rt.disabled) {
        CHECK(!strcmp(pgwt_rt_state_name(&rt), "disabled") && warnings == 0 &&
              (policy[1] & ~PGWT_RT_RESET) == PGWT_RT_OTHER && !soft_limit,
              "disabled under FIFO stays normal without watchdog");
    } else {
        CHECK(!strcmp(pgwt_rt_state_name(&rt), "active") && warnings == 0 && !blocked,
              "success active and SIGXCPU unblocked only on main");
        CHECK(policy[1] == (PGWT_RT_FIFO | PGWT_RT_RESET) && priority[1] == 1,
              "main FIFO 1 with RESET");
        CHECK((policy[2] & ~PGWT_RT_RESET) == PGWT_RT_OTHER && priority[2] == 0,
              "all other threads SCHED_OTHER");
        CHECK(soft_limit == 123000 && hard_limit == 456000 && signal_handler,
              "configurable watchdog installed before FIFO");
        /* Model clone's scheduler reset for a thread created after activation. */
        policy[3] = policy[1] & PGWT_RT_RESET ? PGWT_RT_OTHER : policy[1];
        priority[3] = policy[1] & PGWT_RT_RESET ? 0 : priority[1]; total = 3;
        CHECK(policy[3] == PGWT_RT_OTHER && priority[3] == 0, "future threads SCHED_OTHER");
        if (!strcmp(arm, "signal") || !strcmp(arm, "demote-fails")) {
            fail_demotion = !strcmp(arm, "demote-fails");
            errno = EDOM;
            signal_handler(SIGXCPU);
            CHECK(errno == EDOM, "handler preserves errno");
            if (fail_demotion) {
                CHECK(rt.demote_errno == EPERM && rt.demotions == 0 &&
                      rt.state == PGWT_RT_ACTIVE && hard_limit == 456000,
                      "failed demotion leaves hard watchdog armed, never false success");
            } else {
                int before = set_calls;
                signal_handler(SIGXCPU);
                CHECK(set_calls == before && warnings == 1, "SIGXCPU demotes and warns exactly once");
                CHECK((policy[1] & ~PGWT_RT_RESET) == PGWT_RT_OTHER && priority[1] == 0,
                      "SIGXCPU actually demotes spinning main");
                CHECK(!strcmp(pgwt_rt_state_name(&rt), "demoted") && rt.demotions == 1,
                      "demotion recorded in state and metric");
                CHECK(strstr(message, "123000 us") && strstr(message, "without blocking"),
                      "demotion warning names nonblocking runtime");
                pgwt_rt_pause(&rt); pgwt_rt_start(&rt);
                CHECK(rt.state == PGWT_RT_DEMOTED &&
                      (policy[1] & ~PGWT_RT_RESET) == PGWT_RT_OTHER && warnings == 1,
                      "reconnect never promotes after demotion");
                puts("CONTINUES: capture loop after SIGXCPU");
            }
        }
    }
    puts("PASS");
    return 0;
}
