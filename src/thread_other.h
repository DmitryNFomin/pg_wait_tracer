#ifndef PGWT_THREAD_OTHER_H
#define PGWT_THREAD_OTHER_H

#include <pthread.h>
#include <sched.h>
#include <signal.h>

/* Explicit normal policy, including workers created after RT activation.
 * Block SIGXCPU across creation so only main handles the RT watchdog. */
static inline int pgwt_thread_create_other(pthread_t *thread,
                                           void *(*entry)(void *), void *arg)
{
    pthread_attr_t attr;
    struct sched_param param = { .sched_priority = 0 };
    sigset_t set, previous;
    int rc = pthread_attr_init(&attr);
    if (rc) return rc;
    if ((rc = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED)) == 0 &&
        (rc = pthread_attr_setschedpolicy(&attr, SCHED_OTHER)) == 0 &&
        (rc = pthread_attr_setschedparam(&attr, &param)) == 0) {
        sigemptyset(&set);
        sigaddset(&set, SIGXCPU);
        rc = pthread_sigmask(SIG_BLOCK, &set, &previous);
        if (!rc) {
            rc = pthread_create(thread, &attr, entry, arg);
            (void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
        }
    }
    pthread_attr_destroy(&attr);
    return rc;
}
#endif
