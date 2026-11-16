#ifndef PGWT_TRANSPORT_BUDGET_H
#define PGWT_TRANSPORT_BUDGET_H

#include <stdbool.h>
#include <stdint.h>

/* Shared by a daemon iteration, including nested transport/capture calls.
 * A yield is scheduling only: it never proves completeness. */
#define PGWT_TRANSPORT_PASS_NS UINT64_C(50000000)
#define PGWT_TRANSPORT_DRAIN_NS UINT64_C(10000000)
#define PGWT_TRANSPORT_FINISH_NS UINT64_C(100000000)
struct pgwt_transport_budget {
    uint64_t started, until;
    bool active;
};
static inline void pgwt_budget_begin(struct pgwt_transport_budget *b,
                                    uint64_t now, uint64_t limit)
{
    b->started = now;
    b->until = UINT64_MAX - now < PGWT_TRANSPORT_PASS_NS ? UINT64_MAX : now + PGWT_TRANSPORT_PASS_NS;
    if (limit && limit < b->until) b->until = limit;
    b->active = true;
}
static inline bool pgwt_budget_expired(const struct pgwt_transport_budget *b, uint64_t now)
{
    return b->active && (now < b->started || now >= b->until);
}
#endif
