#ifndef PGWT_A2_POPULATION_H
#define PGWT_A2_POPULATION_H

#include "pgwt_discovery.h"

/* The population gate consumed by the capture/A2 owner. This is not the A2
 * cohort/finality/limits evaluator. Passing this gate supplies no certificate
 * or numerical coverage; failure is Unavailable/untraced_invalid when base
 * coverage otherwise permits numbers (coverage-a2-v1, Population/Status).
 * Identity comes from retained lifecycle history, never a current PID read. */
typedef struct {
    uint64_t pid, cookie, fork_ns;
    bool fork_observed, discovery;
} pgwt_a2_lifetime;
typedef struct {
    uint64_t pid, cookie, fork_ns, exit_ns, receive_ns;
} pgwt_a2_omission;
typedef enum {
    PGWT_A2_POPULATION_OK, PGWT_A2_UNTRACED_INVALID
} pgwt_a2_population_verdict;

static inline bool pgwt_a2_observed_birth(const pgwt_a2_lifetime *l, uint64_t boot_start)
{
    return l && !l->discovery && l->fork_observed && l->cookie &&
        l->cookie < PGWT_DISCOVERY_COOKIE_MIN && l->fork_ns >= boot_start;
}

static inline pgwt_a2_population_verdict pgwt_a2_omission_population(
    const pgwt_a2_lifetime *l, uint64_t boot_start, const pgwt_a2_omission *u)
{
    if (!u || !pgwt_a2_observed_birth(l, boot_start) ||
        u->pid != l->pid || u->cookie != l->cookie || u->fork_ns != l->fork_ns ||
        u->fork_ns > u->exit_ns || u->fork_ns > u->receive_ns)
        return PGWT_A2_UNTRACED_INVALID;
    return PGWT_A2_POPULATION_OK;
}

#endif
