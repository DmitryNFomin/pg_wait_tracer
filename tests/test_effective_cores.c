/* test_effective_cores.c -- fixture-only AAS-1 capacity resolver tests */
#include "effective_cores.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TARGET_PID 4242
#define FIXTURE(name) "fixtures/effective_cores/" name

static int failures;

#define CHECK(cond, fmt, ...) do {                                         \
    if (!(cond)) {                                                         \
        fprintf(stderr, "FAIL %s:%d: " fmt "\n", __func__, __LINE__,       \
                ##__VA_ARGS__);                                            \
        failures++;                                                        \
    }                                                                      \
} while (0)

struct affinity_fixture {
    long values[8];
    size_t value_count;
    size_t next;
};

static int fixture_affinity(pid_t pid, void *context, size_t *cpu_count)
{
    struct affinity_fixture *fixture = context;
    CHECK(pid == TARGET_PID, "target pid %d, expected %d", pid, TARGET_PID);
    if (fixture->next >= fixture->value_count)
        return -1;
    long value = fixture->values[fixture->next++];
    if (value <= 0)
        return -1;
    *cpu_count = (size_t)value;
    return 0;
}

static struct pgwt_effective_cores_result
resolve(const char *fixture_root, long affinity)
{
    struct affinity_fixture af = {
        .values = { affinity },
        .value_count = 1,
    };
    struct pgwt_effective_cores_options options = {
        .filesystem_root = fixture_root,
        .affinity_count = fixture_affinity,
        .affinity_context = &af,
    };
    return pgwt_effective_cores(TARGET_PID, &options);
}

static void check_result(struct pgwt_effective_cores_result result,
                         double cores,
                         enum pgwt_effective_cores_source source)
{
    CHECK(fabs(result.cores - cores) < 1e-12,
          "cores %.12g, expected %.12g", result.cores, cores);
    CHECK(result.source == source, "source %s, expected %s",
          pgwt_effective_cores_source_name(result.source),
          pgwt_effective_cores_source_name(source));
    CHECK(!result.materially_changed, "one-shot resolve reported a change");
}

static void test_v2_affinity_and_quota(void)
{
    printf("--- v2 affinity/quota intersection ---\n");
    check_result(resolve(FIXTURE("v2_unlimited"), 4), 4.0,
                 PGWT_EFFECTIVE_CORES_AFFINITY);
    check_result(resolve(FIXTURE("v2_quota_2_5"), 8), 2.5,
                 PGWT_EFFECTIVE_CORES_QUOTA);
    check_result(resolve(FIXTURE("v2_quota_8"), 2), 2.0,
                 PGWT_EFFECTIVE_CORES_AFFINITY);
}

static void test_v2_hierarchy_and_cpuset(void)
{
    printf("--- v2 hierarchy/cpuset/ignored weight ---\n");
    /* Leaf is unlimited; its parent is the binding 1.5-core quota. */
    check_result(resolve(FIXTURE("v2_ancestor_1_5"), 8), 1.5,
                 PGWT_EFFECTIVE_CORES_QUOTA);
    /* cpuset.cpus.effective grants 0-2,4. cpu.weight=1 is irrelevant. */
    check_result(resolve(FIXTURE("v2_cpuset_4"), 8), 4.0,
                 PGWT_EFFECTIVE_CORES_AFFINITY);
}

static void test_unlimited_v1_and_v2(void)
{
    printf("--- v1/v2 unlimited quotas and ignored shares ---\n");
    check_result(resolve(FIXTURE("v2_unlimited"), 6), 6.0,
                 PGWT_EFFECTIVE_CORES_AFFINITY);
    /* Every v1 quota is -1; cpu.shares=2 must not reduce capacity. */
    check_result(resolve(FIXTURE("v1_unlimited"), 6), 6.0,
                 PGWT_EFFECTIVE_CORES_AFFINITY);
    /* Also prove the v1 ancestor walk and a fractional quota. */
    check_result(resolve(FIXTURE("v1_ancestor_1_75"), 8), 1.75,
                 PGWT_EFFECTIVE_CORES_QUOTA);
}

static void test_failures_do_not_guess(void)
{
    printf("--- failed discovery returns UNKNOWN ---\n");
    struct pgwt_effective_cores_result malformed =
        resolve(FIXTURE("v2_malformed"), 4);
    CHECK(malformed.source == PGWT_EFFECTIVE_CORES_SOURCE_UNKNOWN,
          "malformed quota source=%s",
          pgwt_effective_cores_source_name(malformed.source));
    CHECK(malformed.cores == PGWT_EFFECTIVE_CORES_UNKNOWN,
          "malformed quota cores=%.12g", malformed.cores);

    struct pgwt_effective_cores_result missing =
        resolve(FIXTURE("v2_missing"), 4);
    CHECK(missing.source == PGWT_EFFECTIVE_CORES_SOURCE_UNKNOWN,
          "missing quota source=%s",
          pgwt_effective_cores_source_name(missing.source));
    CHECK(missing.cores == PGWT_EFFECTIVE_CORES_UNKNOWN,
          "missing quota cores=%.12g", missing.cores);

    struct pgwt_effective_cores_result bad_proc =
        resolve(FIXTURE("malformed_proc"), 4);
    CHECK(bad_proc.source == PGWT_EFFECTIVE_CORES_SOURCE_UNKNOWN,
          "malformed /proc cgroup source=%s",
          pgwt_effective_cores_source_name(bad_proc.source));

    struct affinity_fixture af = {
        .values = { -1 },
        .value_count = 1,
    };
    struct pgwt_effective_cores_options options = {
        .filesystem_root = FIXTURE("v2_unlimited"),
        .affinity_count = fixture_affinity,
        .affinity_context = &af,
    };
    struct pgwt_effective_cores_result no_affinity =
        pgwt_effective_cores(TARGET_PID, &options);
    CHECK(no_affinity.source == PGWT_EFFECTIVE_CORES_SOURCE_UNKNOWN,
          "failed affinity source=%s",
          pgwt_effective_cores_source_name(no_affinity.source));
    CHECK(no_affinity.cores == PGWT_EFFECTIVE_CORES_UNKNOWN,
          "failed affinity cores=%.12g", no_affinity.cores);
}

static void test_refresh_change_reporting(void)
{
    printf("--- periodic refresh material-change reporting ---\n");
    struct affinity_fixture af = {
        .values = { 4, 4, 3, -1, -1, 3 },
        .value_count = 6,
    };
    struct pgwt_effective_cores_options options = {
        .filesystem_root = FIXTURE("v2_unlimited"),
        .affinity_count = fixture_affinity,
        .affinity_context = &af,
    };
    struct pgwt_effective_cores_resolver resolver;
    pgwt_effective_cores_resolver_init(&resolver, &options);

    struct pgwt_effective_cores_result r =
        pgwt_effective_cores_refresh(&resolver, TARGET_PID);
    CHECK(r.cores == 4.0 && !r.materially_changed,
          "first refresh cores=%.1f changed=%d", r.cores,
          r.materially_changed);
    r = pgwt_effective_cores_refresh(&resolver, TARGET_PID);
    CHECK(r.cores == 4.0 && !r.materially_changed,
          "stable refresh cores=%.1f changed=%d", r.cores,
          r.materially_changed);
    r = pgwt_effective_cores_refresh(&resolver, TARGET_PID);
    CHECK(r.cores == 3.0 && r.materially_changed,
          "hotplug refresh cores=%.1f changed=%d", r.cores,
          r.materially_changed);
    r = pgwt_effective_cores_refresh(&resolver, TARGET_PID);
    CHECK(r.source == PGWT_EFFECTIVE_CORES_SOURCE_UNKNOWN &&
          r.materially_changed, "known->unknown did not report change");
    r = pgwt_effective_cores_refresh(&resolver, TARGET_PID);
    CHECK(r.source == PGWT_EFFECTIVE_CORES_SOURCE_UNKNOWN &&
          !r.materially_changed, "stable unknown reported change");
    r = pgwt_effective_cores_refresh(&resolver, TARGET_PID);
    CHECK(r.cores == 3.0 && r.materially_changed,
          "unknown->known did not report change");
}

static void test_override_wins(void)
{
    printf("--- explicit override wins and names its metric source ---\n");
    struct affinity_fixture af = {
        .values = { -1 },
        .value_count = 1,
    };
    struct pgwt_effective_cores_options options = {
        .filesystem_root = FIXTURE("malformed_proc"),
        .affinity_count = fixture_affinity,
        .affinity_context = &af,
        .override_cores = 3.25,
    };
    struct pgwt_effective_cores_result result =
        pgwt_effective_cores(TARGET_PID, &options);
    check_result(result, 3.25, PGWT_EFFECTIVE_CORES_OVERRIDE);
    CHECK(af.next == 0, "override still called affinity provider");
    CHECK(strcmp(pgwt_effective_cores_source_name(result.source), "override") == 0,
          "override metric source string changed");
}

int main(void)
{
    test_v2_affinity_and_quota();
    test_v2_hierarchy_and_cpuset();
    test_unlimited_v1_and_v2();
    test_failures_do_not_guess();
    test_refresh_change_reporting();
    test_override_wins();

    if (failures) {
        fprintf(stderr, "\n%d effective-core test(s) FAILED\n", failures);
        return 1;
    }
    printf("\nAll effective-core tests passed.\n");
    return 0;
}
