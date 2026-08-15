/* test_query_text.c — Phase T5 regression tests (DUR-4/10)
 *
 *   DUR-4:  query_texts.jsonl is append-opened + deduped-on-load, never
 *           truncated (retained traces reference the ids across restarts);
 *           compaction only under an explicit size threshold, atomic, and
 *           never dropping untracked lines.
 *   DUR-10: the 4096-id table capping out is logged (visible via the
 *           cap_logged flag here) and the file gets trace-file permissions.
 *
 * Links src/query_text.c directly — no BPF, no daemon.
 */
#include "query_text.h"

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

#define TEST_DIR "/tmp/pgwt_query_text_test"

static void rm_rf(const char *dir)
{
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) { /* ignore */ }
}

static const char *jsonl_path(void)
{
    static char p[300];
    snprintf(p, sizeof(p), "%s/query_texts.jsonl", TEST_DIR);
    return p;
}

static const char *sources_path(void)
{
    static char p[300];
    snprintf(p, sizeof(p), "%s/query_sources.jsonl", TEST_DIR);
    return p;
}

static int count_file_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = 0, c;
    while ((c = fgetc(f)) != EOF)
        if (c == '\n') n++;
    fclose(f);
    return n;
}

static int count_lines(void)
{
    return count_file_lines(jsonl_path());
}

static void test_append_and_dedup(void)
{
    printf("--- DUR-4: append-open + dedup-on-load ---\n");
    rm_rf(TEST_DIR);
    mkdir(TEST_DIR, 0755);

    struct pgwt_query_text_capture *qt = calloc(1, sizeof(*qt));

    /* First run: capture 3 ids. */
    CHECK(pgwt_qt_init(qt, TEST_DIR, 0, 0, (gid_t)-1) == 0, "init 1");
    pgwt_qt_store(qt, 101, "SELECT 1", 0);
    pgwt_qt_store(qt, 102, "SELECT 2", 0);
    pgwt_qt_store(qt, 103, "SELECT 3 -- with \"quotes\"\nand newline", 0);
    pgwt_qt_close(qt);
    CHECK(count_lines() == 3, "3 lines after first run (got %d)",
          count_lines());

    /* Restart: the old code truncated here — the whole DUR-4 bug. */
    memset(qt, 0, sizeof(*qt));
    CHECK(pgwt_qt_init(qt, TEST_DIR, 0, 0, (gid_t)-1) == 0, "init 2");
    CHECK(qt->num_seen == 3, "3 ids loaded from disk (got %d)", qt->num_seen);
    CHECK(count_lines() == 3, "restart did NOT truncate (got %d lines)",
          count_lines());

    /* Re-storing a known id must not duplicate; a new id must append. */
    pgwt_qt_store(qt, 102, "SELECT 2", 0);
    CHECK(count_lines() == 3, "known id not re-written (got %d)",
          count_lines());
    pgwt_qt_store(qt, 104, "SELECT 4", 0);
    CHECK(count_lines() == 4, "new id appended (got %d)", count_lines());
    pgwt_qt_close(qt);

    /* Negative (signed) query_id round-trips through load. */
    memset(qt, 0, sizeof(*qt));
    CHECK(pgwt_qt_init(qt, TEST_DIR, 0, 0, (gid_t)-1) == 0, "init 3");
    pgwt_qt_store(qt, (uint64_t)(int64_t)-5555, "SELECT -1", 0);
    pgwt_qt_close(qt);
    memset(qt, 0, sizeof(*qt));
    CHECK(pgwt_qt_init(qt, TEST_DIR, 0, 0, (gid_t)-1) == 0, "init 4");
    int before = count_lines();
    pgwt_qt_store(qt, (uint64_t)(int64_t)-5555, "SELECT -1", 0);
    CHECK(count_lines() == before, "negative id deduped on load");
    pgwt_qt_close(qt);
    free(qt);
}

static void test_torn_line_tolerated(void)
{
    printf("--- DUR-4: torn last line (crash mid-append) ---\n");
    /* Append a torn line (no trailing newline, invalid JSON). */
    FILE *f = fopen(jsonl_path(), "a");
    fprintf(f, "{\"q\":\"99");
    fclose(f);

    struct pgwt_query_text_capture *qt = calloc(1, sizeof(*qt));
    CHECK(pgwt_qt_init(qt, TEST_DIR, 0, 0, (gid_t)-1) == 0,
          "init over torn line");
    CHECK(qt->num_seen >= 5, "intact ids still loaded (got %d)",
          qt->num_seen);
    pgwt_qt_close(qt);
    free(qt);
}

static void test_compaction(void)
{
    printf("--- DUR-4: compaction under explicit threshold ---\n");
    rm_rf(TEST_DIR);
    mkdir(TEST_DIR, 0755);

    /* Build a file with duplicate lines (pre-dedup-era format). */
    FILE *f = fopen(jsonl_path(), "w");
    for (int round = 0; round < 3; round++)
        for (int id = 1; id <= 10; id++)
            fprintf(f, "{\"q\":\"%d\",\"t\":\"SELECT %d -- round %d\","
                    "\"ts\":1}\n", id, id, round);
    fclose(f);
    long size_before = 0;
    {
        struct stat st;
        stat(jsonl_path(), &st);
        size_before = (long)st.st_size;
    }

    /* Below threshold: no compaction. */
    struct pgwt_query_text_capture *qt = calloc(1, sizeof(*qt));
    CHECK(pgwt_qt_init(qt, TEST_DIR, 0, 0, (gid_t)-1) == 0, "init");
    CHECK(count_lines() == 30, "below threshold: file untouched (got %d)",
          count_lines());
    CHECK(qt->num_seen == 10, "10 unique ids loaded (got %d)", qt->num_seen);
    pgwt_qt_close(qt);

    /* Force the threshold below the file size: compaction must dedup to
     * first occurrences, atomically. */
    setenv("PGWT_QT_COMPACT_BYTES", "64", 1);
    memset(qt, 0, sizeof(*qt));
    CHECK(pgwt_qt_init(qt, TEST_DIR, 0, 0, (gid_t)-1) == 0, "init compact");
    unsetenv("PGWT_QT_COMPACT_BYTES");
    CHECK(count_lines() == 10, "compacted to unique ids (got %d)",
          count_lines());
    struct stat st;
    stat(jsonl_path(), &st);
    CHECK((long)st.st_size < size_before, "file shrank");
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", jsonl_path());
    CHECK(access(tmp, F_OK) != 0, "no tmp file left behind");

    /* First occurrence wins: round 0 text kept. */
    f = fopen(jsonl_path(), "r");
    char line[256];
    int first_ok = 0;
    if (fgets(line, sizeof(line), f))
        first_ok = strstr(line, "round 0") != NULL;
    fclose(f);
    CHECK(first_ok, "first-seen text kept by compaction");

    /* Capture still works after compaction. */
    pgwt_qt_store(qt, 999, "SELECT 999", 0);
    CHECK(count_lines() == 11, "append works after compaction (got %d)",
          count_lines());
    pgwt_qt_close(qt);
    free(qt);
}

static void test_cap_logged(void)
{
    printf("--- DUR-10: id-table cap is loud (log-once flag) ---\n");
    rm_rf(TEST_DIR);
    mkdir(TEST_DIR, 0755);

    struct pgwt_query_text_capture *qt = calloc(1, sizeof(*qt));
    CHECK(pgwt_qt_init(qt, TEST_DIR, 0, 0, (gid_t)-1) == 0, "init");

    fprintf(stderr, "  (expect one WARN about the id table below)\n");
    for (uint64_t id = 1; id <= QT_HT_SIZE + 5; id++)
        pgwt_qt_store(qt, id, "SELECT cap", 0);
    CHECK(qt->num_seen == QT_HT_SIZE, "table filled (got %d)", qt->num_seen);
    CHECK(qt->cap_logged, "cap logged once");
    CHECK(count_lines() == QT_HT_SIZE, "exactly table-size lines (got %d)",
          count_lines());
    pgwt_qt_close(qt);
    free(qt);
}

static void test_permissions(void)
{
    printf("--- DUR-10: file gets trace-file permissions ---\n");
    rm_rf(TEST_DIR);
    mkdir(TEST_DIR, 0755);

    struct pgwt_query_text_capture *qt = calloc(1, sizeof(*qt));
    /* Use our own gid — always permitted. */
    CHECK(pgwt_qt_init(qt, TEST_DIR, 0, 0, getgid()) == 0, "init with gid");
    pgwt_qt_store(qt, 1, "SELECT 1", 0);
    struct stat st;
    CHECK(stat(jsonl_path(), &st) == 0, "file exists");
    CHECK((st.st_mode & 07777) == 0640,
          "mode 0640 like trace files (got %o)", st.st_mode & 07777);
    pgwt_qt_close(qt);
    free(qt);
}

static void test_source_precedence_and_context(void)
{
    printf("--- Stage 4: source precedence + context keys ---\n");
    rm_rf(TEST_DIR);
    mkdir(TEST_DIR, 0755);
    struct pgwt_query_text_capture *qt = calloc(1, sizeof(*qt));
    CHECK(pgwt_qt_init(qt, TEST_DIR, 0, 0, (gid_t)-1) == 0, "init");
    struct pgwt_query_text_key a = {
        .databaseid = 5, .userid = 10, .query_id = 123,
    };
    struct pgwt_query_text_key b = {
        .databaseid = 6, .userid = 10, .query_id = 123,
    };
    pgwt_qt_store_pgss(qt, &a, "select $1");
    pgwt_qt_store_full(qt, &a, "SELECT 42", 999);
    pgwt_qt_store_pgss(qt, &a, "select $2");
    pgwt_qt_store_pgss(qt, &b, "select $1 from other_db");
    CHECK(count_lines() == 3,
          "raw upgrade appends, lower sampled source cannot overwrite, and "
          "same qid in another context is distinct (got %d)", count_lines());
    CHECK(qt->num_seen == 1 && qt->num_sampled_seen == 2,
          "raw and sampled contexts use separate admission tables "
          "(raw=%d sampled=%d)", qt->num_seen, qt->num_sampled_seen);
    pgwt_qt_close(qt);

    memset(qt, 0, sizeof(*qt));
    CHECK(pgwt_qt_init(qt, TEST_DIR, 0, 0, (gid_t)-1) == 0,
          "source-aware restart load");
    int before = count_lines();
    pgwt_qt_store_pgss(qt, &a, "must not replace raw");
    CHECK(count_lines() == before,
          "full/raw precedence survives restart");
    pgwt_qt_close(qt);
    free(qt);
}

static void test_sampled_capacity_cannot_block_raw(void)
{
    printf("--- Stage 4: sampled capacity reserves raw first-seen slots ---\n");
    rm_rf(TEST_DIR);
    mkdir(TEST_DIR, 0755);
    struct pgwt_query_text_capture *qt = calloc(1, sizeof(*qt));
    CHECK(pgwt_qt_init(qt, TEST_DIR, 0, 0, (gid_t)-1) == 0, "init");
    char text[64];
    for (uint64_t i = 1; i <= QT_HT_SIZE; i++) {
        struct pgwt_query_text_key key = {
            .databaseid = 10, .userid = 20, .query_id = i,
        };
        snprintf(text, sizeof(text), "select $1 /* %llu */",
                 (unsigned long long)i);
        pgwt_qt_store_pgss(qt, &key, text);
    }
    struct pgwt_query_text_key raw = {
        .databaseid = 99, .userid = 77, .query_id = 9000001,
    };
    CHECK(qt->num_sampled_seen == QT_HT_SIZE,
          "sampled table reaches its independent cap");
    CHECK(pgwt_qt_store_text(qt, &raw, PGWT_QT_SOURCE_FULL,
                             "SELECT 42", 1234) == 0 && qt->num_seen == 1,
          "FULL/raw capture succeeds after sampled table saturation");
    struct pgwt_query_text_key synth = {
        .databaseid = 99, .userid = 77, .query_id = 9000002,
    };
    pgwt_qt_store_synthetic(qt, &synth, "SELECT ?");
    pgwt_qt_test_drain_synthetic(qt);
    CHECK(qt->num_sampled_seen == QT_HT_SIZE &&
          qt->quality_count == 1 && count_file_lines(sources_path()) == 1,
          "synthetic quality persists after sampled text admission saturates");
    pgwt_qt_close(qt);

    memset(qt, 0, sizeof(*qt));
    CHECK(pgwt_qt_init(qt, TEST_DIR, 0, 0, (gid_t)-1) == 0 &&
          qt->quality_count == 1,
          "synthetic quality dedup reloads independently of text admission");
    pgwt_qt_store_synthetic(qt, &synth, "SELECT ?");
    pgwt_qt_test_drain_synthetic(qt);
    CHECK(count_file_lines(sources_path()) == 1,
          "reloaded synthetic quality is not duplicated");
    pgwt_qt_close(qt);
    free(qt);
}

static void test_synthetic_async_and_quality_bound(void)
{
    printf("--- Stage 4: PG13 persistence is async and bounded ---\n");
    rm_rf(TEST_DIR);
    mkdir(TEST_DIR, 0755);
    struct pgwt_query_text_capture *qt = calloc(1, sizeof(*qt));
    CHECK(pgwt_qt_init(qt, TEST_DIR, 0, 0, (gid_t)-1) == 0, "init");

    struct pgwt_query_text_key first = {
        .databaseid = 5, .userid = 10, .query_id = 1,
    };
    pgwt_qt_test_pause_synthetic(qt, true);
    pgwt_qt_store_synthetic(qt, &first, "SELECT ?");
    CHECK(count_lines() == 0 && count_file_lines(sources_path()) == 0 &&
          qt->num_sampled_seen == 0 && qt->quality_count == 0,
          "sampler call only enqueues; worker owns file I/O and quality work");
    pgwt_qt_test_pause_synthetic(qt, false);
    pgwt_qt_test_drain_synthetic(qt);
    CHECK(count_lines() == 1 && count_file_lines(sources_path()) == 1 &&
          pgwt_qt_has_text(qt, &first),
          "worker persists synthetic text and publishes lock-free presence");

    for (uint64_t id = 2; id <= QT_QUALITY_SIZE * 3; id++) {
        struct pgwt_query_text_key key = {
            .databaseid = 5, .userid = 10, .query_id = id,
        };
        pgwt_qt_store_synthetic(qt, &key, "SELECT ?");
        if ((id & 127) == 0)
            pgwt_qt_test_drain_synthetic(qt);
    }
    pgwt_qt_test_drain_synthetic(qt);
    struct stat st = {0};
    CHECK(qt->quality_capacity == QT_QUALITY_SIZE &&
          qt->quality_count == QT_QUALITY_SIZE,
          "quality_seen stays at its fixed cap and evicts");
    CHECK(stat(sources_path(), &st) == 0 &&
          st.st_size <= QT_QUALITY_MAX_BYTES,
          "query_sources sidecar is compacted within its byte bound");
    pgwt_qt_close(qt);
    free(qt);
}

int main(void)
{
    printf("=== test_query_text (T5: DUR-4/10) ===\n");
    test_append_and_dedup();
    test_torn_line_tolerated();
    test_compaction();
    test_cap_logged();
    test_permissions();
    test_source_precedence_and_context();
    test_sampled_capacity_cannot_block_raw();
    test_synthetic_async_and_quality_bound();
    rm_rf(TEST_DIR);
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
