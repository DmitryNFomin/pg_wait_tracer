/* Stage 3 exact-probe bundle: policy, atomic attach, generations, refcount. */
#include "exact_probe.h"

#include <stdio.h>
#include <string.h>

static int passed, total;
#define CHECK(cond, msg) do { \
    total++; \
    if (cond) { passed++; } else { fprintf(stderr, "FAIL: %s\n", msg); } \
} while (0)

struct fake_ops {
    int attach_calls[PGWT_EXACT_PROBE_COUNT];
    int detach_calls[PGWT_EXACT_PROBE_COUNT];
    int detach_order[32];
    int detach_count;
    int fail_at;
};

static void *fake_attach(void *ctx, enum pgwt_exact_probe_index index)
{
    struct fake_ops *f = ctx;
    f->attach_calls[index]++;
    if (f->fail_at == (int)index)
        return NULL;
    return (void *)(uintptr_t)(index + 1);
}

static void fake_detach(void *ctx, enum pgwt_exact_probe_index index,
                        void *link)
{
    struct fake_ops *f = ctx;
    (void)link;
    f->detach_calls[index]++;
    f->detach_order[f->detach_count++] = index;
}

static const struct pgwt_exact_probe_ops ops = {
    .attach = fake_attach,
    .detach = fake_detach,
};

static struct PgBackendStatusLayout validated_layout(int major)
{
    struct PgBackendStatusLayout l;
    memset(&l, 0, sizeof(l));
    l.major = major;
    l.validation = PGWT_PGBS_VALIDATION_VALIDATED;
    l.st_changecount.validation = PGWT_PGBS_FIELD_VALIDATED;
    l.st_procpid.validation = PGWT_PGBS_FIELD_VALIDATED;
    l.st_state.validation = PGWT_PGBS_FIELD_VALIDATED;
    l.st_query_id.validation = PGWT_PGBS_FIELD_VALIDATED;
    l.st_query_id.present = true;
    return l;
}

static void test_policy(void)
{
    struct PgBackendStatusLayout valid = validated_layout(17);
    struct PgBackendStatusLayout degraded = valid;
    degraded.validation = PGWT_PGBS_VALIDATION_DEGRADED;
    CHECK(pgwt_exact_probe_startup_mask(PGWT_MODE_SAMPLED, 17, &valid,
                                       false, false) == 0,
          "validated PG17 sampled has zero exact links");
    CHECK(pgwt_exact_probe_startup_mask(PGWT_MODE_TIERED, 17, &valid,
                                       false, false) == 0,
          "validated PG17 tiered baseline has zero exact links");
    CHECK(pgwt_exact_probe_startup_mask(PGWT_MODE_SAMPLED, 17, &degraded,
                                       false, false) ==
              PGWT_EXACT_PROBE_ATTR_MASK,
          "degraded PG17 keeps query/activity links");
    CHECK(pgwt_exact_probe_startup_mask(PGWT_MODE_TIERED, 13, &degraded,
                                       false, false) ==
              PGWT_EXACT_PROBE_ATTR_MASK,
          "PG13 keeps query/activity links");
    CHECK(pgwt_exact_probe_startup_mask(PGWT_MODE_FULL, 17, &valid,
                                       false, false) ==
              PGWT_EXACT_PROBE_ALL_MASK,
          "full mode pins every exact link");
    CHECK(pgwt_exact_probe_startup_mask(PGWT_MODE_FULL, 17, &valid,
                                       true, false) == 0,
          "lightweight mode preserves no-query-probe policy");
}

static void test_idempotent_pin_and_partial_rollback(void)
{
    struct pgwt_exact_probe_core core = {0};
    struct fake_ops f = {.fail_at = -1};
    CHECK(pgwt_exact_probe_core_pin(&core, PGWT_EXACT_PROBE_ATTR_MASK, 0,
                                    &ops, &f) == 0,
          "baseline attribution pin succeeds");
    CHECK(pgwt_exact_probe_core_pin(&core, PGWT_EXACT_PROBE_ATTR_MASK, 0,
                                    &ops, &f) == 0,
          "baseline pin is idempotent");
    CHECK(f.attach_calls[0] == 1 && f.attach_calls[1] == 1,
          "idempotent pin never double-attaches");

    CHECK(pgwt_exact_probe_core_acquire(&core, PGWT_EXACT_PROBE_ALL_MASK, 7,
                                        &ops, &f) == 0,
          "pinned attribution bundle is reusable by an exact generation");
    CHECK(f.attach_calls[0] == 1 && f.attach_calls[1] == 1,
          "generation acquire does not double-attach pinned links");
    pgwt_exact_probe_core_release(&core, &ops, &f);
    pgwt_exact_probe_core_cleanup(&core, &ops, &f);
}

static void test_empty_rollback_and_refcount(void)
{
    struct pgwt_exact_probe_core core = {0};
    struct fake_ops f = {.fail_at = PGWT_EXACT_PROBE_ACTIVITY};
    CHECK(pgwt_exact_probe_core_acquire(&core, PGWT_EXACT_PROBE_ALL_MASK, 1,
                                        &ops, &f) != 0,
          "second-link failure denies empty-bundle acquire");
    CHECK(core.attached_mask == 0 && f.detach_calls[0] == 1,
          "empty-bundle partial attach is fully rolled back");

    memset(&f, 0, sizeof(f));
    f.fail_at = -1;
    CHECK(pgwt_exact_probe_core_acquire(&core, PGWT_EXACT_PROBE_ALL_MASK, 11,
                                        &ops, &f) == 0,
          "fresh generation acquire succeeds");
    CHECK(pgwt_exact_probe_core_acquire(&core, PGWT_EXACT_PROBE_ALL_MASK, 11,
                                        &ops, &f) == 0 && core.consumers == 2,
          "same-generation acquire is reference-counted and idempotent");
    CHECK(pgwt_exact_probe_core_acquire(&core, PGWT_EXACT_PROBE_ALL_MASK, 12,
                                        &ops, &f) != 0,
          "different generation cannot share live links");
    pgwt_exact_probe_core_release(&core, &ops, &f);
    CHECK(core.attached_mask == PGWT_EXACT_PROBE_ALL_MASK &&
          core.consumers == 1,
          "first release leaves links for remaining exact consumer");
    pgwt_exact_probe_core_release(&core, &ops, &f);
    CHECK(core.attached_mask == 0 && core.consumers == 0,
          "last exact consumer detaches the bundle");
    CHECK(f.detach_order[f.detach_count - 1] == PGWT_EXACT_PROBE_QUERY_ID,
          "bundle detaches in reverse attach order");
}

static void test_generation_merge(void)
{
    struct pgwt_exact_config cfg = {
        .generation = 7,
        .attach_boundary_ns = 100,
    };
    struct pgwt_exact_seed seed = {
        .generation = 7,
        .snapshot_ts = 130,
        .query_id = 111,
        .cmd_open = 1,
    };
    struct pgwt_exact_attr edge = {
        .query_generation = 6,
        .query_edge_ts = 150,
        .query_id = 222,
        .cmd_generation = 7,
        .cmd_edge_ts = 99,
        .cmd_open = 0,
    };
    uint64_t qid = 0;
    uint16_t open = 0;

    pgwt_exact_merge_attr(&cfg, &edge, &seed, &qid, &open);
    CHECK(qid == 111 && open == 1,
          "stale-generation and pre-boundary edges cannot replace the seed");

    edge.query_generation = 7;
    edge.query_edge_ts = 101;
    edge.cmd_generation = 7;
    edge.cmd_edge_ts = 120;
    pgwt_exact_merge_attr(&cfg, &edge, &seed, &qid, &open);
    CHECK(qid == 222 && open == 0,
          "same-generation post-attach edges override the userspace seed");

    seed.generation = 8;
    edge.query_generation = 6;
    edge.cmd_generation = 6;
    pgwt_exact_merge_attr(&cfg, &edge, &seed, &qid, &open);
    CHECK(qid == 0 && open == 0,
          "a preseed from another generation is never reused");

    cfg.generation = 0;
    pgwt_exact_merge_attr(&cfg, &edge, &seed, &qid, &open);
    CHECK(qid == edge.query_id && open == edge.cmd_open,
          "generation zero preserves the PG13/degraded edge source");
}

int main(void)
{
    test_policy();
    test_idempotent_pin_and_partial_rollback();
    test_empty_rollback_and_refcount();
    test_generation_merge();
    printf("%d/%d checks passed\n", passed, total);
    return passed == total ? 0 : 1;
}
