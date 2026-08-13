/* test_plan_tree.c — Unit tests for plan tree sidecar & NodeTag resolution (F-C4)
 *
 * Validates:
 * 1. NodeTag operator name resolution across PostgreSQL 13-18.
 * 2. Plan node label formatting (relation, alias, index, workers).
 * 3. plan_trees.jsonl append, dedup-on-load, permissions (0640), and table saturation warning (DUR-10).
 *
 * Compiles src/plan_tree.c directly — BPF-free, daemon-free.
 */

#include "plan_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL(%d): " fmt "\n", __LINE__, ##__VA_ARGS__); } \
} while (0)

#define TEST_DIR "/tmp/pgwt_plan_tree_test"

static void rm_rf(const char *dir)
{
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) { /* ignore */ }
}

static const char *jsonl_path(void)
{
    static char p[300];
    snprintf(p, sizeof(p), "%s/plan_trees.jsonl", TEST_DIR);
    return p;
}

static int count_lines(void)
{
    FILE *f = fopen(jsonl_path(), "r");
    if (!f) return -1;
    int n = 0, c;
    while ((c = fgetc(f)) != EOF)
        if (c == '\n')
            n++;
    fclose(f);
    return n;
}

static void test_nodetag_resolution_across_versions(void)
{
    printf("--- F-C4: NodeTag resolution across PG 13-18 ---\n");

    /* PG13 */
    CHECK(strcmp(pgwt_nodetag_name(254, 13), "Seq Scan") == 0, "PG13 Seq Scan");
    CHECK(strcmp(pgwt_nodetag_name(272, 13), "Hash Join") == 0, "PG13 Hash Join");
    CHECK(strcmp(pgwt_nodetag_name(280, 13), "Gather") == 0, "PG13 Gather");

    /* PG14 */
    CHECK(strcmp(pgwt_nodetag_name(275, 14), "Seq Scan") == 0, "PG14 Seq Scan");
    CHECK(strcmp(pgwt_nodetag_name(294, 14), "Hash Join") == 0, "PG14 Hash Join");
    CHECK(strcmp(pgwt_nodetag_name(302, 14), "Gather") == 0, "PG14 Gather");

    /* PG15 */
    CHECK(strcmp(pgwt_nodetag_name(294, 15), "Seq Scan") == 0, "PG15 Seq Scan");
    CHECK(strcmp(pgwt_nodetag_name(313, 15), "Hash Join") == 0, "PG15 Hash Join");
    CHECK(strcmp(pgwt_nodetag_name(321, 15), "Gather") == 0, "PG15 Gather");

    /* PG16 */
    CHECK(strcmp(pgwt_nodetag_name(315, 16), "Seq Scan") == 0, "PG16 Seq Scan");
    CHECK(strcmp(pgwt_nodetag_name(334, 16), "Hash Join") == 0, "PG16 Hash Join");
    CHECK(strcmp(pgwt_nodetag_name(343, 16), "Gather") == 0, "PG16 Gather");

    /* PG17 */
    CHECK(strcmp(pgwt_nodetag_name(335, 17), "Seq Scan") == 0, "PG17 Seq Scan");
    CHECK(strcmp(pgwt_nodetag_name(355, 17), "Hash Join") == 0, "PG17 Hash Join");
    CHECK(strcmp(pgwt_nodetag_name(364, 17), "Gather") == 0, "PG17 Gather");

    /* PG18 */
    CHECK(strcmp(pgwt_nodetag_name(340, 18), "Seq Scan") == 0, "PG18 Seq Scan");
    CHECK(strcmp(pgwt_nodetag_name(360, 18), "Hash Join") == 0, "PG18 Hash Join");
    CHECK(strcmp(pgwt_nodetag_name(369, 18), "Gather") == 0, "PG18 Gather");
}

static void test_label_formatting(void)
{
    printf("--- F-C4: Label formatting ---\n");
    struct pgwt_plan_node n;
    char buf[256];

    memset(&n, 0, sizeof(n));
    snprintf(n.type, sizeof(n.type), "Seq Scan");
    snprintf(n.rel, sizeof(n.rel), "users");
    snprintf(n.alias, sizeof(n.alias), "u");
    pgwt_pt_format_label(&n, buf, sizeof(buf));
    CHECK(strcmp(buf, "Seq Scan on users (u)") == 0, "label with rel and alias (got '%s')", buf);

    memset(&n, 0, sizeof(n));
    snprintf(n.type, sizeof(n.type), "Index Scan");
    snprintf(n.rel, sizeof(n.rel), "orders");
    snprintf(n.index_name, sizeof(n.index_name), "orders_pkey");
    pgwt_pt_format_label(&n, buf, sizeof(buf));
    CHECK(strcmp(buf, "Index Scan on orders using orders_pkey") == 0, "label with index (got '%s')", buf);

    memset(&n, 0, sizeof(n));
    snprintf(n.type, sizeof(n.type), "Gather");
    n.workers = 4;
    pgwt_pt_format_label(&n, buf, sizeof(buf));
    CHECK(strcmp(buf, "Gather (4 workers)") == 0, "label with workers (got '%s')", buf);
}

static void test_sidecar_append_and_dedup(void)
{
    printf("--- F-C4 & DUR-4: plan_trees.jsonl append & dedup ---\n");
    rm_rf(TEST_DIR);
    mkdir(TEST_DIR, 0755);

    struct pgwt_plan_tree_capture *pt = calloc(1, sizeof(*pt));
    CHECK(pgwt_pt_init(pt, TEST_DIR, 17, 0, (gid_t)-1) == 0, "init 1");

    struct pgwt_plan_node nodes[2];
    memset(nodes, 0, sizeof(nodes));
    nodes[0].id = 0;
    nodes[0].tag = 364; /* Gather PG17 */
    snprintf(nodes[0].type, sizeof(nodes[0].type), "Gather");
    nodes[0].workers = 2;
    nodes[0].left_id = 1;
    nodes[0].has_left = 1;

    nodes[1].id = 1;
    nodes[1].tag = 335; /* Seq Scan PG17 */
    snprintf(nodes[1].type, sizeof(nodes[1].type), "Seq Scan");
    snprintf(nodes[1].rel, sizeof(nodes[1].rel), "lineitem");

    /* Store first query plan */
    pgwt_pt_store_nodes(pt, 1000, 101, 1000000000ULL, nodes, 2);
    /* Store second query plan */
    pgwt_pt_store_nodes(pt, 1000, 102, 1000000000ULL, nodes, 1);
    pgwt_pt_close(pt);

    CHECK(count_lines() == 2, "2 lines stored initially");

    /* Reopen without truncation */
    memset(pt, 0, sizeof(*pt));
    CHECK(pgwt_pt_init(pt, TEST_DIR, 17, 0, (gid_t)-1) == 0, "init 2");
    CHECK(pt->num_seen == 2, "2 query_ids loaded from disk");
    CHECK(count_lines() == 2, "restart did not truncate");

    /* Add third query plan */
    pgwt_pt_store_nodes(pt, 1000, 103, 1000000000ULL, nodes, 1);
    pgwt_pt_close(pt);

    CHECK(count_lines() == 3, "3 lines after append");
    free(pt);
}

static void test_cap_logged(void)
{
    printf("--- F-C4 & DUR-10: id table cap warning ---\n");
    rm_rf(TEST_DIR);
    mkdir(TEST_DIR, 0755);

    struct pgwt_plan_tree_capture *pt = calloc(1, sizeof(*pt));
    CHECK(pgwt_pt_init(pt, TEST_DIR, 17, 0, (gid_t)-1) == 0, "init for cap");

    fprintf(stderr, "  (expect one WARN about the plan-tree id table below)\n");
    for (uint64_t id = 1; id <= PGWT_PT_HT_SIZE + 5; id++) {
        struct pgwt_plan_node n = {.id = 0, .tag = 335};
        pgwt_pt_store_nodes(pt, 1000, id, 1000000000ULL, &n, 1);
    }
    CHECK(pt->num_seen == PGWT_PT_HT_SIZE, "table filled to capacity");
    CHECK(pt->cap_logged, "cap logged flag set");
    pgwt_pt_close(pt);
    free(pt);
}

static void test_permissions(void)
{
    printf("--- F-C4 & DUR-10: file gets 0640 permissions ---\n");
    rm_rf(TEST_DIR);
    mkdir(TEST_DIR, 0755);

    struct pgwt_plan_tree_capture *pt = calloc(1, sizeof(*pt));
    CHECK(pgwt_pt_init(pt, TEST_DIR, 17, 0, getgid()) == 0, "init with gid");
    struct pgwt_plan_node n = {.id = 0, .tag = 335};
    pgwt_pt_store_nodes(pt, 1000, 1, 1000000000ULL, &n, 1);

    struct stat st;
    CHECK(stat(jsonl_path(), &st) == 0, "plan_trees.jsonl exists");
    CHECK((st.st_mode & 07777) == 0640,
          "mode 0640 like trace files (got %o)", st.st_mode & 07777);
    pgwt_pt_close(pt);
    free(pt);
}

static void test_torn_line_recovery(void)
{
    printf("--- F-C4 & DUR-4: torn line recovery during reload ---\n");
    rm_rf(TEST_DIR);
    mkdir(TEST_DIR, 0755);

    FILE *f = fopen(jsonl_path(), "w");
    /* Line 1: valid */
    fprintf(f, "{\"q\":\"501\",\"pid\":1000,\"ts\":1000000000,\"nodes\":[{\"id\":0,\"tag\":335,\"type\":\"Seq Scan\"}]}\n");
    /* Line 2: torn / corrupted JSON */
    fprintf(f, "{\"q\":\"502\",\"pid\":1000,\"ts\":1000000000,\"nodes\":[{\"id\":0\n");
    /* Line 3: valid */
    fprintf(f, "{\"q\":\"503\",\"pid\":1000,\"ts\":1000000000,\"nodes\":[{\"id\":0,\"tag\":364,\"type\":\"Gather\"}]}\n");
    fclose(f);

    struct pgwt_plan_tree_capture *pt = calloc(1, sizeof(*pt));
    CHECK(pgwt_pt_init(pt, TEST_DIR, 17, 0, (gid_t)-1) == 0, "init with torn lines");
    CHECK(pt->num_seen == 2, "valid query_ids loaded, torn line skipped");
    pgwt_pt_close(pt);
    free(pt);
}

static void test_compaction(void)
{
    printf("--- F-C4 & DUR-4: bounded compaction under threshold ---\n");
    rm_rf(TEST_DIR);
    mkdir(TEST_DIR, 0755);

    FILE *f = fopen(jsonl_path(), "w");
    /* Write duplicates */
    for (int i = 0; i < 5; i++) {
        fprintf(f, "{\"q\":\"601\",\"pid\":1000,\"ts\":1000000000,\"nodes\":[{\"id\":0,\"tag\":335,\"type\":\"Seq Scan\"}]}\n");
    }
    for (int i = 0; i < 3; i++) {
        fprintf(f, "{\"q\":\"602\",\"pid\":1000,\"ts\":1000000000,\"nodes\":[{\"id\":0,\"tag\":364,\"type\":\"Gather\"}]}\n");
    }
    fclose(f);
    CHECK(count_lines() == 8, "8 lines before compaction");

    /* Force compaction with low threshold */
    setenv("PGWT_PT_COMPACT_BYTES", "10", 1);
    struct pgwt_plan_tree_capture *pt = calloc(1, sizeof(*pt));
    CHECK(pgwt_pt_init(pt, TEST_DIR, 17, 0, (gid_t)-1) == 0, "init triggers compaction");
    CHECK(pt->num_seen == 2, "2 unique query_ids seen");
    pgwt_pt_close(pt);
    free(pt);
    unsetenv("PGWT_PT_COMPACT_BYTES");

    CHECK(count_lines() == 2, "compacted to 2 lines");
}

int main(void)
{
    printf("=== test_plan_tree (F-C4: plan_trees.jsonl & NodeTag mapping) ===\n");
    test_nodetag_resolution_across_versions();
    test_label_formatting();
    test_sidecar_append_and_dedup();
    test_cap_logged();
    test_permissions();
    test_torn_line_recovery();
    test_compaction();
    rm_rf(TEST_DIR);
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
