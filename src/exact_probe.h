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
    /* #108: the per-execution phase markers. One bundle member per USDT
     * PAIR (query__execute__start+done, query__plan__start+done) -- the two
     * links of a pair are attached and detached together, because half a
     * pair produces executions that never close. */
    PGWT_EXACT_PROBE_EXEC,
    PGWT_EXACT_PROBE_PLAN,
    PGWT_EXACT_PROBE_COUNT,
};

#define PGWT_EXACT_PROBE_BIT(index_) (1u << (index_))
#define PGWT_EXACT_PROBE_ATTR_MASK \
    (PGWT_EXACT_PROBE_BIT(PGWT_EXACT_PROBE_QUERY_ID) | \
     PGWT_EXACT_PROBE_BIT(PGWT_EXACT_PROBE_ACTIVITY))
/* Phase markers are OPTIONAL members of the bundle: EL8 kernels without USDT
 * semaphore refcounting have always run exact wait/query capture with these
 * unavailable, so their attach failure degrades an escalation window, it
 * never denies one. They are never part of the always-on sampled tier. */
#define PGWT_EXACT_PROBE_PHASE_MASK \
    (PGWT_EXACT_PROBE_BIT(PGWT_EXACT_PROBE_EXEC) | \
     PGWT_EXACT_PROBE_BIT(PGWT_EXACT_PROBE_PLAN))
#define PGWT_EXACT_PROBE_ALL_MASK \
    (PGWT_EXACT_PROBE_ATTR_MASK | PGWT_EXACT_PROBE_PHASE_MASK)

typedef void *(*pgwt_exact_attach_fn)(void *ctx,
                                      enum pgwt_exact_probe_index index);
typedef void (*pgwt_exact_detach_fn)(void *ctx,
                                     enum pgwt_exact_probe_index index,
                                     void *link);

struct pgwt_exact_probe_ops {
    pgwt_exact_attach_fn attach;
    pgwt_exact_detach_fn detach;
};

enum pgwt_exact_attach_outcome {
    PGWT_EXACT_ATTACH_NOT_ATTEMPTED = 0,
    PGWT_EXACT_ATTACH_ATTACHED,
    PGWT_EXACT_ATTACH_FAILED,
    PGWT_EXACT_ATTACH_DISABLED,
};

/* The core is deliberately libbpf-free so idempotency, reference ownership,
 * and partial-attach rollback are deterministic unit-test targets. */
struct pgwt_exact_probe_core {
    void *links[PGWT_EXACT_PROBE_COUNT];
    uint32_t attached_mask;
    /* Latest attempt or policy refusal, retained after release; diagnostics
     * never gate attempts. attached_mask is CURRENT state.
     * errno is 0 on success or when the failing callback supplied no reason.
     * No support claim: even ENOTSUP is an attempt error, not an independently
     * measured kernel/binary capability. Only optional members are published. */
    enum pgwt_exact_attach_outcome attach_outcome[PGWT_EXACT_PROBE_COUNT];
    int attach_errno[PGWT_EXACT_PROBE_COUNT];
    uint32_t pinned_mask;
    uint64_t generation;
    unsigned consumers;
};

struct cJSON;
/* Flat, additive disclosure beside the mask, independent of window fidelity. */
void pgwt_exact_probe_add_attach_status(struct cJSON *root,
                                        const struct pgwt_exact_probe_core *core);

/* One bundle member, two bpf_links: a USDT phase marker is only meaningful
 * as a start/done PAIR. Both attach or neither does, and they detach
 * together, so an escalation window can never leave executions that open
 * and never close (or close and never open). */
struct pgwt_exact_probe_pair {
    void *start;
    void *done;
};

struct pgwt_exact_probe_bundle {
    struct pgwt_exact_probe_core core;
    uint64_t query_offset;
    uint64_t activity_offset;
    bool sampled_warmup_reconciled;
    /* #108: the execute/plan link pairs, owned by the bundle member whose
     * index they belong to. */
    struct pgwt_exact_probe_pair exec_links;
    struct pgwt_exact_probe_pair plan_links;
    /* The #464 canonical-site allowlist is published once per daemon
     * lifetime; re-publishing on every escalation would re-parse the
     * postgres ELF inside the window. */
    bool exec_sites_published;
};

/* #108: the "both links or neither" rule for one USDT start/done pair.
 *
 * PURE (libbpf-free), so the invariant this branch introduces is a unit
 * test rather than something only a live box could exercise -- and the live
 * A/B in tests/test_escalation.sh provably CANNOT see it: a half-attached
 * pair still yields executions > 0, so both sides of that A/B agree while
 * on_exec_done sits live in the sampled tier.
 *
 * PGWT_EXACT_PAIR_START / _DONE are the `which` values handed to the
 * callbacks. attach returns an opaque link or NULL on failure.
 *
 * pgwt_exact_probe_pair_attach: attaches START, then DONE. If either fails,
 * anything already attached is detached and the pair is left ZEROED --
 * returns -1 and never a half pair, because half a pair produces
 * executions that open and never close (or the reverse), which is worse
 * than absent telemetry because it is invisible.
 *
 * pgwt_exact_probe_pair_detach: detaches DONE then START (reverse of
 * attach) and zeroes the pair. Idempotent: a second call is a no-op, so a
 * release and a cleanup cannot double-destroy a link. */
#define PGWT_EXACT_PAIR_START 0
#define PGWT_EXACT_PAIR_DONE  1

typedef void *(*pgwt_exact_pair_attach_fn)(void *ctx, int which);
typedef void (*pgwt_exact_pair_detach_fn)(void *ctx, int which, void *link);

int pgwt_exact_probe_pair_attach(struct pgwt_exact_probe_pair *pair,
                                 pgwt_exact_pair_attach_fn attach,
                                 pgwt_exact_pair_detach_fn detach,
                                 void *ctx);
void pgwt_exact_probe_pair_detach(struct pgwt_exact_probe_pair *pair,
                                  pgwt_exact_pair_detach_fn detach,
                                  void *ctx);

/* #108: forget everything in the bundle that belongs to a DESTROYED BPF
 * skeleton, keeping what survives one.
 *
 * Cleared: the execute/plan link pairs, and exec_sites_published -- the
 * #464 canonical-site allowlist is RUNTIME addresses in a map owned by that
 * skeleton. A daemon-mode re-attach (PostgreSQL restarted) loads a fresh,
 * EMPTY exec_site_allow and may see a different load base, so the publish
 * has to happen again. Keeping the flag would leave the BPF handlers'
 * exec_site_admitted() refusing every address while the links looked
 * healthy: zero executions, silently -- the exact silent-zero shape the
 * allowlist refusal exists to prevent.
 *
 * Kept: query_offset / activity_offset, which are FILE offsets into the
 * postgres binary and do not move when a process restarts. (They do move
 * if the restart is onto a DIFFERENT binary; that is pre-existing and not
 * addressed here.) */
void pgwt_exact_probe_bundle_forget_skeleton(
    struct pgwt_exact_probe_bundle *bundle);

uint32_t pgwt_exact_probe_startup_mask(
    enum pgwt_mode mode, int pg_major,
    const struct PgBackendStatusLayout *layout,
    bool lightweight_mode, bool skip_usdt);

/* Escalation retains attribution probes; flags disable only phase markers. */
uint32_t pgwt_exact_probe_acquire_mask(bool lightweight_mode, bool skip_usdt);

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
