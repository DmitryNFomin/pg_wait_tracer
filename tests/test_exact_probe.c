/* Stage 3 exact-probe bundle: policy, atomic attach, generations, refcount. */
#include "exact_probe.h"
#include "backend_meta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    l.st_databaseid.validation = PGWT_PGBS_FIELD_VALIDATED;
    l.st_userid.validation = PGWT_PGBS_FIELD_VALIDATED;
    l.st_state.validation = PGWT_PGBS_FIELD_VALIDATED;
    l.st_activity_raw.validation = PGWT_PGBS_FIELD_VALIDATED;
    l.st_activity_raw.present = true;
    l.activity_buffer_size = 1024;
    if (major == 13)
        l.status_anchor = 0x1000;
    if (major >= 14) {
        l.st_query_id.validation = PGWT_PGBS_FIELD_VALIDATED;
        l.st_query_id.present = true;
    } else {
        l.st_query_id.validation = PGWT_PGBS_FIELD_ABSENT;
    }
    return l;
}

static void test_policy(void)
{
    struct PgBackendStatusLayout valid = validated_layout(17);
    struct PgBackendStatusLayout degraded = valid;
    degraded.validation = PGWT_PGBS_VALIDATION_DEGRADED;
    struct PgBackendStatusLayout valid13 = validated_layout(13);
    struct PgBackendStatusLayout degraded13 = valid13;
    degraded13.validation = PGWT_PGBS_VALIDATION_DEGRADED;
    struct PgBackendStatusLayout missing_anchor13 = valid13;
    missing_anchor13.status_anchor = 0;
    for (int major = 14; major <= 18; major++) {
        struct PgBackendStatusLayout version_valid = validated_layout(major);
        CHECK(pgwt_exact_probe_startup_mask(
                  PGWT_MODE_SAMPLED, major, &version_valid,
                  false, false) == 0,
              "validated PG14-18 sampled has zero exact links");
        CHECK(pgwt_exact_probe_startup_mask(
                  PGWT_MODE_TIERED, major, &version_valid,
                  false, false) == 0,
              "validated PG14-18 tiered baseline has zero exact links");
    }
    CHECK(pgwt_exact_probe_startup_mask(PGWT_MODE_SAMPLED, 17, &degraded,
                                       false, false) ==
              PGWT_EXACT_PROBE_ATTR_MASK,
          "degraded PG17 keeps query/activity links");
    CHECK(pgwt_exact_probe_startup_mask(PGWT_MODE_TIERED, 13, &valid13,
                                       false, false) == 0,
          "validated PG13 activity-text path has zero sampled exact links");
    CHECK(pgwt_exact_probe_startup_mask(PGWT_MODE_SAMPLED, 13, &valid13,
                                       false, false) == 0,
          "validated PG13 sampled path has zero exact links");
    CHECK(pgwt_exact_probe_startup_mask(PGWT_MODE_TIERED, 13, &degraded13,
                                       false, false) ==
              PGWT_EXACT_PROBE_ATTR_MASK,
          "degraded PG13 keeps query/activity links fail-safe");
    CHECK(pgwt_exact_probe_startup_mask(PGWT_MODE_TIERED, 13,
                                       &missing_anchor13, false, false) ==
              PGWT_EXACT_PROBE_ATTR_MASK,
          "PG13 without a validated shared-array anchor keeps probes fail-safe");
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

static void test_best_effort_startup_keeps_available_probe(void)
{
    struct pgwt_exact_probe_core core = {0};
    struct fake_ops f = {.fail_at = PGWT_EXACT_PROBE_QUERY_ID};
    uint32_t pinned = pgwt_exact_probe_core_pin_best_effort(
        &core, PGWT_EXACT_PROBE_ALL_MASK, 1, &ops, &f);

    CHECK(pinned == PGWT_EXACT_PROBE_BIT(PGWT_EXACT_PROBE_ACTIVITY) &&
          core.attached_mask == pinned && core.pinned_mask == pinned,
          "startup drops a failed probe but keeps the available probe");
    CHECK(f.attach_calls[PGWT_EXACT_PROBE_QUERY_ID] == 1 &&
          f.attach_calls[PGWT_EXACT_PROBE_ACTIVITY] == 1 &&
          f.detach_count == 0,
          "best-effort startup attempts each probe without bundle rollback");

    CHECK(pgwt_exact_probe_core_acquire(&core,
                                        PGWT_EXACT_PROBE_ALL_MASK, 2,
                                        &ops, &f) != 0 &&
          core.consumers == 0 && core.attached_mask == pinned,
          "exact-window acquire remains strict after degraded startup");
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
        .query_quality = PGWT_QUERY_QUALITY_PG13_SYNTH,
    };
    struct pgwt_exact_attr edge = {
        .query_generation = 6,
        .query_edge_ts = 150,
        .query_id = 222,
        .query_quality = PGWT_QUERY_QUALITY_REAL,
        .cmd_generation = 7,
        .cmd_edge_ts = 99,
        .cmd_open = 0,
    };
    uint64_t qid = 0;
    uint16_t open = 0;
    uint16_t quality = 0;
    uint32_t packed = 0;

    pgwt_exact_merge_attr(&cfg, &edge, &seed, &qid, &packed);
    open = packed & 0xffff; quality = packed >> 16;
    CHECK(qid == 111 && open == 1 &&
          quality == PGWT_QUERY_QUALITY_PG13_SYNTH,
          "PG13 straddler keeps its synthetic seed and quality flag");

    edge.query_generation = 7;
    edge.query_edge_ts = 101;
    edge.query_id = 0;
    edge.query_quality = PGWT_QUERY_QUALITY_NONE;
    edge.cmd_generation = 7;
    edge.cmd_edge_ts = 120;
    edge.cmd_open = 0;
    pgwt_exact_merge_attr(&cfg, &edge, &seed, &qid, &packed);
    open = packed & 0xffff; quality = packed >> 16;
    CHECK(qid == 0 && open == 0 && quality == PGWT_QUERY_QUALITY_NONE,
          "PG13 command close clears the straddler synthetic edge");

    edge.cmd_edge_ts = 130;
    edge.cmd_open = 1;
    pgwt_exact_merge_attr(&cfg, &edge, &seed, &qid, &packed);
    open = packed & 0xffff; quality = packed >> 16;
    CHECK(qid == 0 && open == 1 && quality == PGWT_QUERY_QUALITY_NONE,
          "next PG13 command starts unattributed instead of leaking the seed");

    edge.query_edge_ts = 140;
    edge.query_id = 222;
    edge.query_quality = PGWT_QUERY_QUALITY_REAL;
    pgwt_exact_merge_attr(&cfg, &edge, &seed, &qid, &packed);
    open = packed & 0xffff; quality = packed >> 16;
    CHECK(qid == 222 && open == 1 && quality == PGWT_QUERY_QUALITY_REAL,
          "post-attach ExecutorStart transitions synthetic seed to real qid");

    seed.generation = 8;
    edge.query_generation = 6;
    edge.cmd_generation = 6;
    pgwt_exact_merge_attr(&cfg, &edge, &seed, &qid, &packed);
    open = packed & 0xffff; quality = packed >> 16;
    CHECK(qid == 0 && open == 0,
          "a preseed from another generation is never reused");

    cfg.generation = 0;
    pgwt_exact_merge_attr(&cfg, &edge, &seed, &qid, &packed);
    open = packed & 0xffff; quality = packed >> 16;
    CHECK(qid == edge.query_id && open == edge.cmd_open,
          "generation zero preserves the degraded-layout edge source");
}

static void test_straddler_context_persistence(void)
{
    const char *dir = "/tmp/pgwt_exact_context_test";
    (void)system("rm -rf /tmp/pgwt_exact_context_test");
    CHECK(mkdir(dir, 0700) == 0, "create exact-context fixture directory");
    struct pgwt_backend_meta_writer bm = {0};
    CHECK(pgwt_bm_init(&bm, dir) == 0, "open backend context sidecar");
    struct pgwt_backend be = {.pid = 4321, .meta_parsed = true};
    be.meta.backend_type = PGWT_BT_CLIENT;
    snprintf(be.meta.usename, sizeof(be.meta.usename), "u");
    snprintf(be.meta.datname, sizeof(be.meta.datname), "d");
    pgwt_bm_observe_context(&bm, &be, 55, 66);
    pgwt_bm_close(&bm);
    CHECK(be.databaseid == 55 && be.userid == 66,
          "exact preseed caches coherent straddler context immediately");
    char path[256], line[512] = "";
    snprintf(path, sizeof(path), "%s/backends.jsonl", dir);
    FILE *fp = fopen(path, "r");
    if (fp) {
        (void)fgets(line, sizeof(line), fp);
        fclose(fp);
    }
    CHECK(strstr(line, "\"pid\":4321") && strstr(line, "\"dbid\":55") &&
          strstr(line, "\"userid\":66"),
          "exact preseed persists context before a later sampler tick");
    (void)system("rm -rf /tmp/pgwt_exact_context_test");
}

int main(void)
{
    test_policy();
    test_idempotent_pin_and_partial_rollback();
    test_best_effort_startup_keeps_available_probe();
    test_empty_rollback_and_refcount();
    test_generation_merge();
    test_straddler_context_persistence();
    printf("%d/%d checks passed\n", passed, total);
    return passed == total ? 0 : 1;
}
