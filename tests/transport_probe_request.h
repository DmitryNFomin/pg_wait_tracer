/* Untimed per-CPU helper wake channel. The consumer coalesces until frontier
 * acknowledgement; spurious condition wakes never execute a syscall. Refs #871. */
#ifndef NMI_TRANSPORT_REQUEST_H
#define NMI_TRANSPORT_REQUEST_H
#include <stdbool.h>
#include <pthread.h>
struct transport_request_channel {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool initialized, wake, stop;
};
static inline int transport_request_init(struct transport_request_channel *c)
{
    int rc = pthread_mutex_init(&c->mutex, NULL);
    if (rc) return rc;
    rc = pthread_cond_init(&c->condition, NULL);
    if (rc) { pthread_mutex_destroy(&c->mutex); return rc; }
    c->initialized = true;
    return 0;
}
static inline bool transport_request_wait(struct transport_request_channel *c)
{
    pthread_mutex_lock(&c->mutex);
    while (!c->wake && !c->stop) pthread_cond_wait(&c->condition, &c->mutex);
    bool run = !c->stop;
    c->wake = false;
    pthread_mutex_unlock(&c->mutex);
    return run;
}
static inline void transport_request_signal(struct transport_request_channel *c)
{
    pthread_mutex_lock(&c->mutex);
    c->wake = true;
    pthread_cond_signal(&c->condition);
    pthread_mutex_unlock(&c->mutex);
}
static inline void transport_request_stop(struct transport_request_channel *c)
{
    pthread_mutex_lock(&c->mutex);
    c->stop = true;
    pthread_cond_signal(&c->condition);
    pthread_mutex_unlock(&c->mutex);
}
static inline void transport_request_destroy(struct transport_request_channel *c)
{
    if (!c->initialized) return;
    pthread_cond_destroy(&c->condition);
    pthread_mutex_destroy(&c->mutex);
    c->initialized = false;
}
#endif
