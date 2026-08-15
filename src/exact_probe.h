/* exact_probe.h -- Stage 3 tiered exact-probe lifecycle. */
#ifndef PGWT_EXACT_PROBE_H
#define PGWT_EXACT_PROBE_H

#include "backend_status_layout.h"
#include "provider.h"
#include "pg_wait_tracer.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct pgwt_daemon;

enum pgwt_exact_probe_index {
    PGWT_EXACT_PROBE_QUERY_ID = 0,
    PGWT_EXACT_PROBE_ACTIVITY,
    PGWT_EXACT_PROBE_COUNT,
};

#define PGWT_EXACT_PROBE_BIT(index_) (1u << (index_))
#define PGWT_EXACT_PROBE_ATTR_MASK \
    (PGWT_EXACT_PROBE_BIT(PGWT_EXACT_PROBE_QUERY_ID) | \
     PGWT_EXACT_PROBE_BIT(PGWT_EXACT_PROBE_ACTIVITY))
#define PGWT_EXACT_PROBE_ALL_MASK PGWT_EXACT_PROBE_ATTR_MASK

typedef void *(*pgwt_exact_attach_fn)(void *ctx,
                                      enum pgwt_exact_probe_index index);
typedef void (*pgwt_exact_detach_fn)(void *ctx,
                                     enum pgwt_exact_probe_index index,
                                     void *link);

struct pgwt_exact_probe_ops {
    pgwt_exact_attach_fn attach;
    pgwt_exact_detach_fn detach;
};

/* The core is deliberately libbpf-free so idempotency, reference ownership,
 * and partial-attach rollback are deterministic unit-test targets. */
struct pgwt_exact_probe_core {
    void *links[PGWT_EXACT_PROBE_COUNT];
    uint32_t attached_mask;
    uint32_t pinned_mask;
    uint64_t generation;
    unsigned consumers;
};

struct pgwt_exact_probe_bundle {
    struct pgwt_exact_probe_core core;
    uint64_t query_offset;
    uint64_t activity_offset;
    bool sampled_warmup_reconciled;
};

uint32_t pgwt_exact_probe_startup_mask(
    enum pgwt_mode mode, int pg_major,
    const struct PgBackendStatusLayout *layout,
    bool lightweight_mode, bool skip_usdt);

int pgwt_exact_probe_core_pin(struct pgwt_exact_probe_core *core,
                              uint32_t mask, uint64_t generation,
                              const struct pgwt_exact_probe_ops *ops,
                              void *ctx);
uint32_t pgwt_exact_probe_core_pin_best_effort(
    struct pgwt_exact_probe_core *core, uint32_t mask, uint64_t generation,
    const struct pgwt_exact_probe_ops *ops, void *ctx);
int pgwt_exact_probe_core_acquire(struct pgwt_exact_probe_core *core,
                                  uint32_t mask, uint64_t generation,
                                  const struct pgwt_exact_probe_ops *ops,
                                  void *ctx);
void pgwt_exact_probe_core_release(struct pgwt_exact_probe_core *core,
                                   const struct pgwt_exact_probe_ops *ops,
                                   void *ctx);
void pgwt_exact_probe_core_unpin(struct pgwt_exact_probe_core *core,
                                 uint32_t mask,
                                 const struct pgwt_exact_probe_ops *ops,
                                 void *ctx);
void pgwt_exact_probe_core_cleanup(struct pgwt_exact_probe_core *core,
                                   const struct pgwt_exact_probe_ops *ops,
                                   void *ctx);

#ifndef PGWT_SERVER
int pgwt_exact_probe_startup(struct pgwt_daemon *d);
void pgwt_exact_probe_reconcile_sampled(struct pgwt_daemon *d);
int pgwt_exact_probe_acquire(struct pgwt_daemon *d, uint64_t generation,
                             uint64_t attach_boundary_ns);
int pgwt_exact_probe_quiesce(struct pgwt_daemon *d);
void pgwt_exact_probe_release(struct pgwt_daemon *d);
void pgwt_exact_probe_cleanup(struct pgwt_daemon *d);

int pgwt_exact_seed_backend(struct pgwt_daemon *d, pid_t pid,
                            uint64_t generation, bool require_coherent);
int pgwt_exact_seed_all(struct pgwt_daemon *d, uint64_t generation);
void pgwt_exact_seed_clear(struct pgwt_daemon *d, uint64_t generation);
unsigned pgwt_exact_seed_count(struct pgwt_daemon *d, uint64_t generation);

int pgwt_exact_resolve_attr(struct pgwt_daemon *d, pid_t pid,
                            uint64_t *query_id, uint16_t *cmd_open,
                            uint16_t *query_quality,
                            uint16_t *phase_flags,
                            uint64_t *plan_start_ts,
                            uint64_t *exec_start_ts);
uint64_t pgwt_exact_uprobe_fire_count(struct pgwt_daemon *d, uint32_t slot);
#endif

#endif /* PGWT_EXACT_PROBE_H */
