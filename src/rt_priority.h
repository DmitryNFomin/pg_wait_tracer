/* Main-loop scheduling and the kernel nonblocking-CPU watchdog. */
#ifndef PGWT_RT_PRIORITY_H
#define PGWT_RT_PRIORITY_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* Linux policy values, also used by the syscall-injected host tests. */
#define PGWT_RT_OTHER 0
#define PGWT_RT_FIFO 1
#define PGWT_RT_RESET 0x40000000
#define PGWT_RT_SOFT_US 200000UL
#define PGWT_RT_HARD_US 1000000UL

enum pgwt_rt_state { PGWT_RT_DISABLED, PGWT_RT_ACTIVE,
                     PGWT_RT_REFUSED, PGWT_RT_DEMOTED };

struct pgwt_rt_ops {
    pid_t (*tid)(void);
    int (*threads)(int (*visit)(pid_t, void *), void *);
    int (*get)(pid_t, int *policy, int *priority);
    /* Also called in the signal handler: must be async-signal-safe. */
    int (*set)(pid_t, int policy, int priority);
    int (*limits)(unsigned long soft_us, unsigned long hard_us);
    int (*handler)(void (*fn)(int));
    int (*mask)(bool block);
    /* Also called in the signal handler: must be async-signal-safe. */
    void (*warning)(const char *, size_t);
};

struct pgwt_rt_priority {
    const struct pgwt_rt_ops *ops;
    pid_t main_tid;
    volatile sig_atomic_t state;
    volatile sig_atomic_t demotions;
    volatile sig_atomic_t demote_errno;
    bool disabled, prepared, attempted, warned;
    unsigned long soft_us, hard_us;
    char demotion_warning[256];
    size_t demotion_warning_len;
};

extern const struct pgwt_rt_ops pgwt_rt_linux_ops;
void pgwt_rt_init(struct pgwt_rt_priority *, const struct pgwt_rt_ops *);
/* Call before discovery can create workers or fork helper processes. */
void pgwt_rt_prepare(struct pgwt_rt_priority *);
void pgwt_rt_start(struct pgwt_rt_priority *);
void pgwt_rt_pause(struct pgwt_rt_priority *);
const char *pgwt_rt_state_name(const struct pgwt_rt_priority *);

#endif
