/* exact_probe.c -- Stage 3 escalation-only query/activity probes. */
#include "exact_probe.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "synthetic_query.h"

static int attach_missing(struct pgwt_exact_probe_core *core, uint32_t mask,
                          const struct pgwt_exact_probe_ops *ops, void *ctx,
                          uint32_t *new_mask)
{
    *new_mask = 0;
    if (!core || !ops || !ops->attach || !ops->detach)
        return -1;
    for (int i = 0; i < PGWT_EXACT_PROBE_COUNT; i++) {
        uint32_t bit = PGWT_EXACT_PROBE_BIT(i);
        if (!(mask & bit) || (core->attached_mask & bit))
            continue;
        void *link = ops->attach(ctx, (enum pgwt_exact_probe_index)i);
        if (!link) {
            for (int j = PGWT_EXACT_PROBE_COUNT - 1; j >= 0; j--) {
                uint32_t rollback_bit = PGWT_EXACT_PROBE_BIT(j);
                if (!(*new_mask & rollback_bit))
                    continue;
                ops->detach(ctx, (enum pgwt_exact_probe_index)j,
                            core->links[j]);
                core->links[j] = NULL;
                core->attached_mask &= ~rollback_bit;
            }
            *new_mask = 0;
            return -1;
        }
        core->links[i] = link;
        core->attached_mask |= bit;
        *new_mask |= bit;
    }
    return 0;
}

void pgwt_exact_probe_bundle_forget_skeleton(
    struct pgwt_exact_probe_bundle *bundle)
{
    if (!bundle)
        return;
    bundle->exec_links.start = NULL;
    bundle->exec_links.done = NULL;
    bundle->plan_links.start = NULL;
    bundle->plan_links.done = NULL;
    bundle->exec_sites_published = false;
}

uint32_t pgwt_exact_probe_startup_mask(
    enum pgwt_mode mode, int pg_major,
    const struct PgBackendStatusLayout *layout,
    bool lightweight_mode, bool skip_usdt)
{
    if (lightweight_mode || skip_usdt || mode == PGWT_MODE_COOP)
        return 0;
    if (mode == PGWT_MODE_FULL)
        return PGWT_EXACT_PROBE_ALL_MASK;
    bool validated_tick_source = pg_major >= 13 &&
        pgwt_pgbs_sampled_attr_enabled(layout);
    return validated_tick_source ? 0 : PGWT_EXACT_PROBE_ATTR_MASK;
}

int pgwt_exact_probe_core_pin(struct pgwt_exact_probe_core *core,
                              uint32_t mask, uint64_t generation,
                              const struct pgwt_exact_probe_ops *ops,
                              void *ctx)
{
    uint32_t new_mask = 0;
    if (attach_missing(core, mask, ops, ctx, &new_mask) != 0)
        return -1;
    core->pinned_mask |= mask;
    if (generation)
        core->generation = generation;
    return 0;
}

uint32_t pgwt_exact_probe_core_pin_best_effort(
    struct pgwt_exact_probe_core *core, uint32_t mask, uint64_t generation,
    const struct pgwt_exact_probe_ops *ops, void *ctx)
{
    if (!core || !ops || !ops->attach || !ops->detach)
        return 0;

    uint32_t pinned = core->pinned_mask & mask;
    for (int i = 0; i < PGWT_EXACT_PROBE_COUNT; i++) {
        uint32_t bit = PGWT_EXACT_PROBE_BIT(i);
        if (!(mask & bit) || (pinned & bit))
            continue;

        /* A one-bit attach cannot roll back another startup probe.  Exact
         * generation acquisition deliberately continues to use the atomic
         * multi-bit attach_missing() path below. */
        uint32_t new_mask = 0;
        if (attach_missing(core, bit, ops, ctx, &new_mask) != 0)
            continue;
        core->pinned_mask |= bit;
        pinned |= bit;
    }
    if (generation && pinned)
        core->generation = generation;
    return pinned;
}

int pgwt_exact_probe_core_acquire(struct pgwt_exact_probe_core *core,
                                  uint32_t mask, uint64_t generation,
                                  const struct pgwt_exact_probe_ops *ops,
                                  void *ctx)
{
    if (!core || generation == 0)
        return -1;
    if (core->consumers) {
        if (core->generation != generation)
            return -1;
        core->consumers++;
        return 0;
    }
    /* REQUIRED members attach atomically: a partial attribution bundle
     * would hand out an exact generation whose answers are half exact. */
    uint32_t required = mask & ~PGWT_EXACT_PROBE_PHASE_MASK;
    uint32_t new_mask = 0;
    if (attach_missing(core, required, ops, ctx, &new_mask) != 0)
        return -1;
    core->generation = generation;
    core->consumers = 1;

    /* #108: OPTIONAL members (the execute/plan USDT phase markers) attach
     * one at a time, best effort, AFTER the window is already granted.
     * A kernel without USDT semaphore refcounting -- EL8 -- has never been
     * able to carry them, and denying the whole escalation there would
     * remove exact wait capture to protect an optional overlay. They are
     * not pinned, so pgwt_exact_probe_core_release() detaches them with
     * the rest of the window: they never outlive it into the always-on
     * sampled tier. */
    uint32_t optional = mask & PGWT_EXACT_PROBE_PHASE_MASK;
    for (int i = 0; i < PGWT_EXACT_PROBE_COUNT; i++) {
        uint32_t bit = PGWT_EXACT_PROBE_BIT(i);
        if (!(optional & bit) || (core->attached_mask & bit))
            continue;
        uint32_t got = 0;
        (void)attach_missing(core, bit, ops, ctx, &got);
    }
    return 0;
}

void pgwt_exact_probe_core_release(struct pgwt_exact_probe_core *core,
                                   const struct pgwt_exact_probe_ops *ops,
                                   void *ctx)
{
    if (!core || !core->consumers || !ops || !ops->detach)
        return;
    if (--core->consumers)
        return;
    uint32_t transient = core->attached_mask & ~core->pinned_mask;
    for (int i = PGWT_EXACT_PROBE_COUNT - 1; i >= 0; i--) {
        uint32_t bit = PGWT_EXACT_PROBE_BIT(i);
        if (!(transient & bit))
            continue;
        ops->detach(ctx, (enum pgwt_exact_probe_index)i, core->links[i]);
        core->links[i] = NULL;
        core->attached_mask &= ~bit;
    }
    core->generation = 0;
}

void pgwt_exact_probe_core_unpin(struct pgwt_exact_probe_core *core,
                                 uint32_t mask,
                                 const struct pgwt_exact_probe_ops *ops,
                                 void *ctx)
{
    if (!core || !ops || !ops->detach)
        return;
    core->pinned_mask &= ~mask;
    if (core->consumers)
        return;
    uint32_t detachable = core->attached_mask & mask;
    for (int i = PGWT_EXACT_PROBE_COUNT - 1; i >= 0; i--) {
        uint32_t bit = PGWT_EXACT_PROBE_BIT(i);
        if (!(detachable & bit))
            continue;
        ops->detach(ctx, (enum pgwt_exact_probe_index)i, core->links[i]);
        core->links[i] = NULL;
        core->attached_mask &= ~bit;
    }
}

void pgwt_exact_probe_core_cleanup(struct pgwt_exact_probe_core *core,
                                   const struct pgwt_exact_probe_ops *ops,
                                   void *ctx)
{
    if (!core || !ops || !ops->detach)
        return;
    for (int i = PGWT_EXACT_PROBE_COUNT - 1; i >= 0; i--) {
        uint32_t bit = PGWT_EXACT_PROBE_BIT(i);
        if (!(core->attached_mask & bit))
            continue;
        ops->detach(ctx, (enum pgwt_exact_probe_index)i, core->links[i]);
        core->links[i] = NULL;
    }
    memset(core, 0, sizeof(*core));
}

#ifndef PGWT_SERVER
#include "daemon.h"
#include "backend.h"
#include "discovery.h"
#include "sampler.h"

#include <stdio.h>
#include <unistd.h>
#include <time.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include "pg_wait_tracer.skel.h"

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static struct bpf_link **skeleton_link_slot(struct pgwt_daemon *d,
                                            enum pgwt_exact_probe_index index)
{
    switch (index) {
    case PGWT_EXACT_PROBE_QUERY_ID:
        return d->use_pg13_query_attr ? &d->skel->links.on_executor_start
                                      : &d->skel->links.on_report_query_id;
    case PGWT_EXACT_PROBE_ACTIVITY:   return &d->skel->links.on_report_activity;
    default: return NULL;
    }
}

/* #108: attach one USDT start/done PAIR. Both or neither -- half a pair
 * produces executions that open and never close (or the reverse), which is
 * worse than no execution telemetry at all because it is invisible. */
static void *attach_usdt_pair(struct pgwt_daemon *d,
                              struct pgwt_exact_probe_pair *pair,
                              struct bpf_program *prog_start,
                              struct bpf_program *prog_done,
                              struct bpf_link **slot_start,
                              struct bpf_link **slot_done,
                              const char *note_start, const char *note_done)
{
    const char *bin = d->pg_binary_saved;
    struct bpf_link *a = bpf_program__attach_usdt(prog_start, -1, bin,
                                                  "postgresql", note_start,
                                                  NULL);
    if (libbpf_get_error(a))
        a = NULL;
    struct bpf_link *b = NULL;
    if (a) {
        b = bpf_program__attach_usdt(prog_done, -1, bin, "postgresql",
                                     note_done, NULL);
        if (libbpf_get_error(b))
            b = NULL;
    }
    if (!a || !b) {
        if (b) bpf_link__destroy(b);
        if (a) bpf_link__destroy(a);
        errno = ENOTSUP;
        return NULL;
    }
    pair->start = a;
    pair->done = b;
    *slot_start = a;
    *slot_done = b;
    return pair;
}

static void detach_usdt_pair(struct pgwt_exact_probe_pair *pair,
                             struct bpf_link **slot_start,
                             struct bpf_link **slot_done)
{
    if (slot_start) *slot_start = NULL;
    if (slot_done) *slot_done = NULL;
    if (pair->done) bpf_link__destroy(pair->done);
    if (pair->start) bpf_link__destroy(pair->start);
    pair->start = NULL;
    pair->done = NULL;
}

static void *attach_link(void *ctx, enum pgwt_exact_probe_index index)
{
    struct pgwt_daemon *d = ctx;
    const char *bin = d->pg_binary_saved;
    struct bpf_link *link = NULL;
    const char *fail = getenv("PGWT_TEST_EXACT_PROBE_FAIL_AT");
    const char *escalation_fail =
        getenv("PGWT_TEST_EXACT_PROBE_ESCALATION_FAIL_AT");
    bool force_startup_or_exact = fail && atoi(fail) == (int)index;
    /* The escalation-only variant exercises atomic acquisition rollback
     * without corrupting startup capability discovery.  Keep the original
     * unscoped hook for the independent startup-degradation regression test. */
    bool force_escalation = escalation_fail &&
        atoi(escalation_fail) == (int)index &&
        d->escalation.generation != 0;
    if (force_startup_or_exact || force_escalation) {
        errno = EIO;
        fprintf(stderr, "WARN: %s=%d -- forcing exact-link attach failure "
                "(TEST ONLY)\n",
                force_startup_or_exact ? "PGWT_TEST_EXACT_PROBE_FAIL_AT" :
                "PGWT_TEST_EXACT_PROBE_ESCALATION_FAIL_AT",
                index);
        return NULL;
    }
    if (!bin) {
        errno = ENOENT;
        return NULL;
    }

    LIBBPF_OPTS(bpf_uprobe_opts, opts, .retprobe = false);
    switch (index) {
    case PGWT_EXACT_PROBE_QUERY_ID:
        if (!d->exact_probes.query_offset) {
            const char *symbol = d->use_pg13_query_attr
                               ? "standard_ExecutorStart"
                               : "pgstat_report_query_id";
            uint64_t va = pgwt_find_symbol_offset(bin, symbol);
            d->exact_probes.query_offset = pgwt_vaddr_to_file_offset(bin, va);
        }
        if (!d->exact_probes.query_offset) {
            errno = ENOENT;
            break;
        }
        link = bpf_program__attach_uprobe_opts(
            d->use_pg13_query_attr ? d->skel->progs.on_executor_start
                                   : d->skel->progs.on_report_query_id,
            -1, bin, d->exact_probes.query_offset, &opts);
        break;
    case PGWT_EXACT_PROBE_ACTIVITY:
        if (!d->skel->rodata->bs_state_running) {
            errno = ENOTSUP;
            break;
        }
        if (!d->exact_probes.activity_offset) {
            uint64_t va = pgwt_find_symbol_offset(bin,
                                                   "pgstat_report_activity");
            d->exact_probes.activity_offset = pgwt_vaddr_to_file_offset(bin, va);
        }
        if (!d->exact_probes.activity_offset) {
            errno = ENOENT;
            break;
        }
        link = bpf_program__attach_uprobe_opts(
            d->skel->progs.on_report_activity, -1, bin,
            d->exact_probes.activity_offset, &opts);
        break;
    case PGWT_EXACT_PROBE_EXEC:
        if (d->lightweight_mode || d->skip_usdt) {
            errno = ENOTSUP;
            break;
        }
        /* #464: publish the canonical-site allowlist BEFORE attaching.
         * On refusal, attach NOTHING: execution telemetry is absent and
         * loud, never doubled and quiet. Published once per daemon --
         * the map outlives a de-escalation and re-parsing the postgres
         * ELF inside a bounded window is overhead the window does not
         * need. */
        if (!d->exact_probes.exec_sites_published) {
            if (pgwt_publish_exec_sites(d, bin) != 0) {
                errno = ENOTSUP;
                break;
            }
            d->exact_probes.exec_sites_published = true;
        }
        return attach_usdt_pair(d, &d->exact_probes.exec_links,
                                d->skel->progs.on_exec_start,
                                d->skel->progs.on_exec_done,
                                &d->skel->links.on_exec_start,
                                &d->skel->links.on_exec_done,
                                "query__execute__start",
                                "query__execute__done");
    case PGWT_EXACT_PROBE_PLAN:
        if (d->lightweight_mode || d->skip_usdt) {
            errno = ENOTSUP;
            break;
        }
        return attach_usdt_pair(d, &d->exact_probes.plan_links,
                                d->skel->progs.on_plan_start,
                                d->skel->progs.on_plan_done,
                                &d->skel->links.on_plan_start,
                                &d->skel->links.on_plan_done,
                                "query__plan__start",
                                "query__plan__done");
    default:
        break;
    }
    long err = libbpf_get_error(link);
    if (err) {
        errno = (int)-err;
        link = NULL;
    }
    if (!link)
        return NULL;
    struct bpf_link **slot = skeleton_link_slot(d, index);
    if (slot)
        *slot = link;
    return link;
}

static void detach_link(void *ctx, enum pgwt_exact_probe_index index,
                        void *opaque)
{
    struct pgwt_daemon *d = ctx;
    if (index == PGWT_EXACT_PROBE_EXEC) {
        detach_usdt_pair(&d->exact_probes.exec_links,
                         &d->skel->links.on_exec_start,
                         &d->skel->links.on_exec_done);
        return;
    }
    if (index == PGWT_EXACT_PROBE_PLAN) {
        detach_usdt_pair(&d->exact_probes.plan_links,
                         &d->skel->links.on_plan_start,
                         &d->skel->links.on_plan_done);
        return;
    }
    struct bpf_link **slot = skeleton_link_slot(d, index);
    if (slot)
        *slot = NULL;
    if (opaque)
        bpf_link__destroy(opaque);
}

static const struct pgwt_exact_probe_ops live_ops = {
    .attach = attach_link,
    .detach = detach_link,
};

static int set_config(struct pgwt_daemon *d, uint64_t generation,
                      uint64_t boundary, bool admitting)
{
    int fd = bpf_map__fd(d->skel->maps.exact_config_map);
    uint32_t key = 0;
    struct pgwt_exact_config cfg = {
        .generation = generation,
        .attach_boundary_ns = boundary,
        .admitting = admitting,
    };
    return bpf_map_update_elem(fd, &key, &cfg, BPF_ANY);
}

int pgwt_exact_probe_startup(struct pgwt_daemon *d)
{
    uint32_t mask = pgwt_exact_probe_startup_mask(
        d->mode, d->pg_major_version, &d->backend_status_layout,
        d->lightweight_mode != 0, d->skip_usdt != 0);
    bool test_sampled_uprobes =
        getenv("PGWT_TEST_SAMPLED_UPROBES") != NULL &&
        (d->mode == PGWT_MODE_SAMPLED || d->mode == PGWT_MODE_TIERED);
    if (test_sampled_uprobes) {
        mask |= PGWT_EXACT_PROBE_ATTR_MASK;
        fprintf(stderr, "WARN: PGWT_TEST_SAMPLED_UPROBES -- restoring "
                "always-on sampled attribution traps (TEST ONLY)\n");
    }
    uint64_t generation = d->mode == PGWT_MODE_FULL ? 1 : 0;
    if (set_config(d, generation, generation ? mono_ns() : 0, true) != 0)
        return -1;

    /* Startup is a capability boundary, not an exact-window transaction.
     * Master treated these attribution links as optional: missing symbols or
     * attach restrictions reduced attribution while wait capture continued.
     * Keep that contract here.  Escalation still uses the atomic acquire path
     * below and denies a partially instrumented exact generation. */
    uint32_t pinned = pgwt_exact_probe_core_pin_best_effort(
        &d->exact_probes.core, mask, generation, &live_ops, d);
    uint32_t dropped = mask & ~pinned;
    for (int i = 0; i < PGWT_EXACT_PROBE_COUNT; i++) {
        uint32_t bit = PGWT_EXACT_PROBE_BIT(i);
        if (!(dropped & bit))
            continue;
        if (i == PGWT_EXACT_PROBE_EXEC || i == PGWT_EXACT_PROBE_PLAN) {
            fprintf(stderr, "WARN: optional full-mode %s USDT phase markers "
                    "unavailable\n",
                    i == PGWT_EXACT_PROBE_EXEC ? "execution" : "plan");
        } else if (i == PGWT_EXACT_PROBE_QUERY_ID) {
            const char *symbol = d->use_pg13_query_attr
                               ? "standard_ExecutorStart"
                               : "pgstat_report_query_id";
            fprintf(stderr,
                    "WARN: startup exact query-id probe unavailable "
                    "(could not resolve or attach %s); dropping it -- "
                    "query attribution unavailable, continuing wait-only "
                    "capture\n", symbol);
        } else {
            fprintf(stderr,
                    "WARN: startup exact activity probe unavailable "
                    "(could not resolve or attach pgstat_report_activity); "
                    "dropping it and continuing wait capture\n");
        }
    }
    d->cmd_gate_active = pgwt_pgbs_sampled_attr_enabled(
        &d->backend_status_layout) ||
        (d->exact_probes.core.attached_mask &
         PGWT_EXACT_PROBE_BIT(PGWT_EXACT_PROBE_ACTIVITY));
    if (mask == 0 && pgwt_pgbs_sampled_attr_enabled(
                         &d->backend_status_layout))
        fprintf(stderr, "INFO: sampled exact probes detached: validated "
                "PgBackendStatus attribution is authoritative\n");
    else if (!dropped && d->mode == PGWT_MODE_FULL &&
             mask == PGWT_EXACT_PROBE_ALL_MASK)
        fprintf(stderr, "INFO: full exact probe bundle attached from startup\n");
    else if (!dropped && mask == PGWT_EXACT_PROBE_ATTR_MASK)
        fprintf(stderr, "INFO: sampled exact attribution probes pinned "
                "(degraded layout fallback)\n");
    else if (pinned)
        fprintf(stderr, "INFO: startup exact probes degraded: attached "
                "mask=0x%x requested=0x%x\n", pinned, mask);
    return 0;
}

void pgwt_exact_probe_reconcile_sampled(struct pgwt_daemon *d)
{
    if ((d->mode != PGWT_MODE_SAMPLED && d->mode != PGWT_MODE_TIERED) ||
        !pgwt_pgbs_sampled_attr_enabled(&d->backend_status_layout))
        return;
    if (getenv("PGWT_TEST_SAMPLED_UPROBES"))
        return;
    uint32_t before = d->exact_probes.core.attached_mask;
    pgwt_exact_probe_core_unpin(&d->exact_probes.core,
                                PGWT_EXACT_PROBE_ATTR_MASK, &live_ops, d);
    d->cmd_gate_active = true;
    if (before != d->exact_probes.core.attached_mask) {
        d->exact_probes.sampled_warmup_reconciled = true;
        fprintf(stderr, "INFO: detached validation-warmup attribution probes; "
                "coherent PgBackendStatus source is now authoritative\n");
    }
}

int pgwt_exact_probe_acquire(struct pgwt_daemon *d, uint64_t generation,
                             uint64_t attach_boundary_ns)
{
    if (set_config(d, generation, attach_boundary_ns, true) != 0)
        return -1;
    if (pgwt_exact_probe_core_acquire(&d->exact_probes.core,
                                      PGWT_EXACT_PROBE_ALL_MASK, generation,
                                      &live_ops, d) != 0) {
        set_config(d, 0, 0, true);
        return -1;
    }
    /* #108: say which half of the bundle the window actually got. An
     * escalation that grants exactness but silently has no phase markers
     * is exactly the failure this issue was: the Waterfall and Scatter
     * tabs came back empty with nothing anywhere saying why. */
    uint32_t phase = d->exact_probes.core.attached_mask &
                     PGWT_EXACT_PROBE_PHASE_MASK;
    if (phase != PGWT_EXACT_PROBE_PHASE_MASK)
        fprintf(stderr, "WARN: exact window %llu has no %s phase markers "
                "(attached mask 0x%x) -- Waterfall/Scatter executions will "
                "be absent for this window\n",
                (unsigned long long)generation,
                phase == 0 ? "execute or plan"
                           : (phase & PGWT_EXACT_PROBE_BIT(
                                  PGWT_EXACT_PROBE_EXEC) ? "plan"
                                                         : "execute"),
                phase);
    else if (d->verbose)
        fprintf(stderr, "INFO: exact window %llu attached execute/plan "
                "phase markers\n", (unsigned long long)generation);
    d->cmd_gate_active = true;
    return 0;
}

int pgwt_exact_probe_quiesce(struct pgwt_daemon *d)
{
    if (!d->skel)
        return -1;
    int fd = bpf_map__fd(d->skel->maps.exact_config_map);
    uint32_t key = 0;
    struct pgwt_exact_config cfg = {0};
    if (bpf_map_lookup_elem(fd, &key, &cfg) != 0)
        return -1;
    cfg.admitting = 0;
    return bpf_map_update_elem(fd, &key, &cfg, BPF_ANY);
}

void pgwt_exact_probe_release(struct pgwt_daemon *d)
{
    pgwt_exact_probe_core_release(&d->exact_probes.core, &live_ops, d);
    if (d->exact_probes.core.consumers == 0)
        set_config(d, 0, 0, true);
    d->cmd_gate_active = pgwt_pgbs_sampled_attr_enabled(
        &d->backend_status_layout) ||
        (d->exact_probes.core.attached_mask &
         PGWT_EXACT_PROBE_BIT(PGWT_EXACT_PROBE_ACTIVITY));
}

void pgwt_exact_probe_cleanup(struct pgwt_daemon *d)
{
    pgwt_exact_probe_core_cleanup(&d->exact_probes.core, &live_ops, d);
    /* The skeleton is destroyed right after this (src/daemon.c), and a
     * daemon-mode re-attach reuses this same struct pgwt_daemon. */
    pgwt_exact_probe_bundle_forget_skeleton(&d->exact_probes);
}

static int get_config(struct pgwt_daemon *d, struct pgwt_exact_config *cfg)
{
    uint32_t key = 0;
    int fd = bpf_map__fd(d->skel->maps.exact_config_map);
    return bpf_map_lookup_elem(fd, &key, cfg);
}

int pgwt_exact_resolve_attr(struct pgwt_daemon *d, pid_t pid,
                            uint64_t *query_id, uint16_t *cmd_open,
                            uint16_t *query_quality,
                            uint16_t *phase_flags,
                            uint64_t *plan_start_ts,
                            uint64_t *exec_start_ts)
{
    if (query_id) *query_id = 0;
    if (cmd_open) *cmd_open = 0;
    if (query_quality) *query_quality = PGWT_QUERY_QUALITY_NONE;
    if (phase_flags) *phase_flags = 0;
    if (plan_start_ts) *plan_start_ts = 0;
    if (exec_start_ts) *exec_start_ts = 0;
    if (!d->skel || pid <= 0)
        return -1;
    uint32_t key = (uint32_t)pid;
    struct pgwt_exact_config cfg = {0};
    struct pgwt_exact_attr edge = {0};
    struct pgwt_exact_seed seed = {0};
    get_config(d, &cfg);
    bool have_edge = bpf_map_lookup_elem(
        bpf_map__fd(d->skel->maps.exact_attr_map), &key, &edge) == 0;
    bool have_seed = bpf_map_lookup_elem(
        bpf_map__fd(d->skel->maps.exact_seed_map), &key, &seed) == 0;

    uint64_t qid = 0;
    uint16_t open = 0;
    uint16_t quality = 0;
    uint32_t packed = 0;
    pgwt_exact_merge_attr(&cfg, have_edge ? &edge : NULL,
                          have_seed ? &seed : NULL, &qid, &packed);
    open = (uint16_t)(packed & 0xffff);
    quality = (uint16_t)(packed >> 16);
    if (query_id) *query_id = qid;
    if (cmd_open) *cmd_open = open;
    if (query_quality) *query_quality = quality;
    if (have_edge && edge.phase_generation == cfg.generation) {
        if (phase_flags) *phase_flags = edge.phase_flags;
        if (plan_start_ts) *plan_start_ts = edge.plan_start_ts;
        if (exec_start_ts) *exec_start_ts = edge.exec_start_ts;
    }
    return have_edge || have_seed ? 0 : -1;
}

int pgwt_exact_seed_backend(struct pgwt_daemon *d, pid_t pid,
                            uint64_t generation, bool require_coherent)
{
    if (!d->skel || pid <= 0 || generation == 0)
        return -1;
    uint64_t qid = 0;
    uint16_t open = 0;
    uint16_t quality = PGWT_QUERY_QUALITY_NONE;
    bool coherent = false;
    bool activity_source = pgwt_pgbs_sampled_activity_enabled(
        &d->backend_status_layout);
    bool sampled_attr_enabled = pgwt_pgbs_sampled_attr_enabled(
        &d->backend_status_layout);
    if (sampled_attr_enabled && activity_source && !require_coherent) {
        /* PG13 auxiliary processes have no query activity to seed. */
        coherent = true;
    } else if (sampled_attr_enabled) {
        struct pgwt_pgbs_sampled_attr attr;
        struct pgwt_backend *be = pgwt_find_backend(&d->backends, pid);
        int attr_rc;
        if (activity_source) {
            if (be && be->pgbs_addr == 0)
                be->pgbs_addr = pgwt_pgbs_resolve_entry(
                    pid, &d->backend_status_layout);
            attr_rc = be && be->pgbs_addr != 0
                ? pgwt_pgbs_read_sampled_attr_at(
                    pid, be->pgbs_addr, &d->backend_status_layout, &attr)
                : -1;
        } else {
            attr_rc = pgwt_pgbs_read_sampled_attr(
                pid, d->my_be_entry_addr, &d->backend_status_layout, &attr);
        }
        if (attr_rc == 0) {
            if (be)
                pgwt_bm_observe_context(d->backend_meta, be,
                                        attr.databaseid, attr.userid);
            qid = attr.query_id;
            open = attr.cmd_open;
            if (pgwt_pgbs_sampled_activity_enabled(
                    &d->backend_status_layout) && attr.cmd_open) {
                char normalized[PGWT_PG13_SYNTH_TEXT_MAX];
                if (pgwt_pg13_synthetic_query(
                        attr.databaseid, attr.userid, attr.activity,
                        normalized, sizeof(normalized), &qid) == 0) {
                    quality = PGWT_QUERY_QUALITY_PG13_SYNTH;
                    if (d->query_text_capture) {
                        struct pgwt_query_text_key text_key = {
                            .databaseid = attr.databaseid,
                            .userid = attr.userid,
                            .query_id = qid,
                        };
                        pgwt_qt_store_synthetic(d->query_text_capture,
                                                &text_key, normalized);
                    }
                } else {
                    qid = 0;
                }
            } else if (qid) {
                quality = PGWT_QUERY_QUALITY_REAL;
            }
            coherent = true;
        }
    } else {
        /* The config already names the new generation, so the merge correctly
         * rejects the pinned fallback's generation-0 edge. Read that BPF-owned
         * edge directly as the legacy pre-window state, then stamp it into the
         * new generation's separate seed. This preserves a degraded-layout
         * command that was already running when escalation began. */
        uint32_t key = (uint32_t)pid;
        struct pgwt_exact_attr fallback = {0};
        if (bpf_map_lookup_elem(
                bpf_map__fd(d->skel->maps.exact_attr_map),
                &key, &fallback) == 0) {
            qid = fallback.query_id;
            open = fallback.cmd_open;
            quality = qid ? PGWT_QUERY_QUALITY_REAL
                          : PGWT_QUERY_QUALITY_NONE;
        }
        int gate = open;
        pgwt_read_cmd_gate(pid, d->debug_query_string_addr,
                           d->my_be_entry_addr, d->st_query_id_offset,
                           &gate, &qid);
        open = gate ? 1 : 0;
        coherent = true; /* unchanged legacy fallback contract */
    }

    if (!coherent) {
        /* A post-boundary edge is independently authoritative and is enough
         * for a backend that forked during this generation. */
        struct pgwt_exact_config cfg = {0};
        struct pgwt_exact_attr edge = {0};
        uint32_t key = (uint32_t)pid;
        get_config(d, &cfg);
        if (bpf_map_lookup_elem(bpf_map__fd(d->skel->maps.exact_attr_map),
                                &key, &edge) == 0 &&
            ((edge.query_generation == generation &&
              edge.query_edge_ts >= cfg.attach_boundary_ns) ||
             (edge.cmd_generation == generation &&
              edge.cmd_edge_ts >= cfg.attach_boundary_ns))) {
            pgwt_exact_resolve_attr(d, pid, &qid, &open, NULL,
                                    NULL, NULL, NULL);
            quality = qid ? PGWT_QUERY_QUALITY_REAL
                          : PGWT_QUERY_QUALITY_NONE;
            coherent = true;
        }
    }
    if (!coherent && require_coherent)
        return -1;

    uint32_t key = (uint32_t)pid;
    struct pgwt_exact_seed seed = {
        .generation = generation,
        .snapshot_ts = mono_ns(),
        .query_id = qid,
        .cmd_open = open,
        .query_quality = quality,
    };
    return bpf_map_update_elem(bpf_map__fd(d->skel->maps.exact_seed_map),
                               &key, &seed, BPF_ANY);
}

int pgwt_exact_seed_all(struct pgwt_daemon *d, uint64_t generation)
{
    /* Deterministic integration hook for anomaly retry coverage: fail exactly
     * one coherent preseed in this process, after link acquisition but before
     * publishing any generation seed. */
    static bool test_preseed_failed_once;
    if (!test_preseed_failed_once &&
        getenv("PGWT_TEST_EXACT_PRESEED_FAIL_ONCE")) {
        test_preseed_failed_once = true;
        fprintf(stderr, "WARN: PGWT_TEST_EXACT_PRESEED_FAIL_ONCE -- forcing "
                "one coherent preseed rollback (TEST ONLY)\n");
        return -1;
    }

    for (int i = 0; i < d->backends.count; i++) {
        struct pgwt_backend *be = &d->backends.entries[i];
        if (!be->is_alive || be->pid <= 0)
            continue;
        /* Sampled forks are registered before PostgreSQL rewrites argv, and
         * escalation may arrive before the next sampler tick classifies one.
         * Retry now so an auxiliary child (notably PG13's logger) is not
         * mistaken for an attribution-bearing unknown backend. */
        if (!be->meta_parsed && pgwt_parse_cmdline(be->pid, &be->meta) == 0)
            be->meta_parsed = true;
        bool needs_attr = !be->meta_parsed ||
            be->meta.backend_type == PGWT_BT_CLIENT ||
            be->meta.backend_type == PGWT_BT_PARALLEL_WORKER;
        if (pgwt_exact_seed_backend(d, be->pid, generation, needs_attr) != 0) {
            char proc_path[64];
            snprintf(proc_path, sizeof(proc_path), "/proc/%d", be->pid);
            if (access(proc_path, F_OK) == 0) {
                fprintf(stderr, "WARN: exact generation %llu denied: cannot "
                        "coherently preseed live PID %d\n",
                        (unsigned long long)generation, be->pid);
                return -1;
            }
        }
    }
    return 0;
}

void pgwt_exact_seed_clear(struct pgwt_daemon *d, uint64_t generation)
{
    if (!d->skel)
        return;
    int fd = bpf_map__fd(d->skel->maps.exact_seed_map);
    uint32_t keys[MAX_BACKENDS];
    unsigned count = 0;
    uint32_t key, next;
    int rc = bpf_map_get_next_key(fd, NULL, &next);
    while (rc == 0 && count < MAX_BACKENDS) {
        struct pgwt_exact_seed seed;
        key = next;
        if (bpf_map_lookup_elem(fd, &key, &seed) == 0 &&
            seed.generation == generation)
            keys[count++] = key;
        rc = bpf_map_get_next_key(fd, &key, &next);
    }
    for (unsigned i = 0; i < count; i++)
        bpf_map_delete_elem(fd, &keys[i]);
}

unsigned pgwt_exact_seed_count(struct pgwt_daemon *d, uint64_t generation)
{
    if (!d->skel)
        return 0;
    int fd = bpf_map__fd(d->skel->maps.exact_seed_map);
    unsigned count = 0;
    uint32_t key, next;
    int rc = bpf_map_get_next_key(fd, NULL, &next);
    while (rc == 0) {
        struct pgwt_exact_seed seed;
        key = next;
        if (bpf_map_lookup_elem(fd, &key, &seed) == 0 &&
            (!generation || seed.generation == generation))
            count++;
        rc = bpf_map_get_next_key(fd, &key, &next);
    }
    return count;
}

uint64_t pgwt_exact_uprobe_fire_count(struct pgwt_daemon *d, uint32_t slot)
{
    if (!d->skel || slot >= PGWT_UPROBE_FIRE_MAX)
        return 0;
    int fd = bpf_map__fd(d->skel->maps.uprobe_fire_counts);
    int cpus = libbpf_num_possible_cpus();
    if (fd < 0 || cpus <= 0)
        return 0;
    uint64_t values[cpus];
    if (bpf_map_lookup_elem(fd, &slot, values) != 0)
        return 0;
    uint64_t total = 0;
    for (int i = 0; i < cpus; i++)
        total += values[i];
    return total;
}
#endif /* !PGWT_SERVER */
