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

static void test_traceparent_parsing(void)
{
    printf("--- F-C1: W3C traceparent comment parsing ---\n");
    char tid[36] = {0};
    char psid[20] = {0};

    /* Standard unquoted */
    const char *q1 = "/*traceparent=00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01*/ SELECT 1";
    CHECK(pgwt_parse_traceparent(q1, tid, sizeof(tid), psid, sizeof(psid)) == 1, "unquoted traceparent");
    CHECK(strcmp(tid, "4bf92f3577b34da6a3ce929d0e0e4736") == 0, "tid match (got %s)", tid);
    CHECK(strcmp(psid, "00f067aa0ba902b7") == 0, "psid match (got %s)", psid);

    /* Single quoted (SQLCommenter standard) */
    const char *q2 = "/*traceparent='00-60b6417fa7216a6552a4be6058e5ec9b-325b138ff40c1d29-01'*/ SELECT 2";
    CHECK(pgwt_parse_traceparent(q2, tid, sizeof(tid), psid, sizeof(psid)) == 1, "single-quoted traceparent");
    CHECK(strcmp(tid, "60b6417fa7216a6552a4be6058e5ec9b") == 0, "tid match (got %s)", tid);
    CHECK(strcmp(psid, "325b138ff40c1d29") == 0, "psid match (got %s)", psid);

    /* Double quoted */
    const char *q3 = "/*traceparent=\"00-a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4-1234567890abcdef-00\"*/ SELECT 3";
    CHECK(pgwt_parse_traceparent(q3, tid, sizeof(tid), psid, sizeof(psid)) == 1, "double-quoted traceparent");
    CHECK(strcmp(tid, "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4") == 0, "tid match (got %s)", tid);
    CHECK(strcmp(psid, "1234567890abcdef") == 0, "psid match (got %s)", psid);

    /* Colon separator with spaces */
    const char *q4 = "/* traceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01 */ SELECT 4";
    CHECK(pgwt_parse_traceparent(q4, tid, sizeof(tid), psid, sizeof(psid)) == 1, "colon traceparent");
    CHECK(strcmp(tid, "4bf92f3577b34da6a3ce929d0e0e4736") == 0, "tid match (got %s)", tid);
    CHECK(strcmp(psid, "00f067aa0ba902b7") == 0, "psid match (got %s)", psid);

    /* Mixed SQLCommenter tags */
    const char *q5 = "/*action='list',controller='orders',traceparent='00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01'*/ SELECT 5";
    CHECK(pgwt_parse_traceparent(q5, tid, sizeof(tid), psid, sizeof(psid)) == 1, "mixed tags traceparent");
    CHECK(strcmp(tid, "4bf92f3577b34da6a3ce929d0e0e4736") == 0, "tid match (got %s)", tid);
    CHECK(strcmp(psid, "00f067aa0ba902b7") == 0, "psid match (got %s)", psid);

    /* Invalid: all-zero trace ID (W3C violation) */
    const char *q6 = "/*traceparent='00-00000000000000000000000000000000-00f067aa0ba902b7-01'*/ SELECT 6";
    CHECK(pgwt_parse_traceparent(q6, tid, sizeof(tid), psid, sizeof(psid)) == 0, "reject all-zero tid");

    /* Invalid: all-zero span ID (W3C violation) */
    const char *q7 = "/*traceparent='00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01'*/ SELECT 7";
    CHECK(pgwt_parse_traceparent(q7, tid, sizeof(tid), psid, sizeof(psid)) == 0, "reject all-zero span id");

    /* Invalid: non-hex characters */
    const char *q8 = "/*traceparent='00-4bf92f3577b34da6a3ce929d0e0e473g-00f067aa0ba902b7-01'*/ SELECT 8";
    CHECK(pgwt_parse_traceparent(q8, tid, sizeof(tid), psid, sizeof(psid)) == 0, "reject non-hex");

    /* Invalid: truncated */
    const char *q9 = "/*traceparent='00-4bf92f3577b34da6a3ce929d0e0e4736-00f0'*/ SELECT 9";
    CHECK(pgwt_parse_traceparent(q9, tid, sizeof(tid), psid, sizeof(psid)) == 0, "reject truncated");

    /* Uppercase hex normalization */
    const char *q11 = "/*traceparent='00-4BF92F3577B34DA6A3CE929D0E0E4736-00F067AA0BA902B7-01'*/ SELECT 11";
    CHECK(pgwt_parse_traceparent(q11, tid, sizeof(tid), psid, sizeof(psid)) == 1, "uppercase hex traceparent");
    CHECK(strcmp(tid, "4bf92f3577b34da6a3ce929d0e0e4736") == 0, "tid normalized lowercase (got %s)", tid);
    CHECK(strcmp(psid, "00f067aa0ba902b7") == 0, "psid normalized lowercase (got %s)", psid);

    /* Multi-line SQL comment with formatting */
    const char *q12 = "/*\n * application: backend\n * traceparent='00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00'\n */ SELECT 12";
    CHECK(pgwt_parse_traceparent(q12, tid, sizeof(tid), psid, sizeof(psid)) == 1, "multiline comment traceparent");
    CHECK(strcmp(tid, "4bf92f3577b34da6a3ce929d0e0e4736") == 0, "tid match (got %s)", tid);
    CHECK(strcmp(psid, "00f067aa0ba902b7") == 0, "psid match (got %s)", psid);

    /* Trace flag 02 */
    const char *q13 = "/*traceparent='00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-02'*/ SELECT 13";
    CHECK(pgwt_parse_traceparent(q13, tid, sizeof(tid), psid, sizeof(psid)) == 1, "flag 02 traceparent");

    /* Plain query without traceparent */
    const char *q10 = "SELECT 10";
    CHECK(pgwt_parse_traceparent(q10, tid, sizeof(tid), psid, sizeof(psid)) == 0, "plain query no traceparent");
}

static void test_traceparent_jsonl_output(void)
{
    printf("--- F-C1: traceparent serialization in query_texts.jsonl ---\n");
    rm_rf(TEST_DIR);
    mkdir(TEST_DIR, 0755);

    struct pgwt_query_text_capture *qt = calloc(1, sizeof(*qt));
    CHECK(pgwt_qt_init(qt, TEST_DIR, 0, 0, (gid_t)-1) == 0, "init for traceparent jsonl");

    const char *q_with_trace = "/*traceparent='00-60b6417fa7216a6552a4be6058e5ec9b-325b138ff40c1d29-01'*/ SELECT * FROM users";
    const char *q_no_trace = "SELECT count(*) FROM orders";

    pgwt_qt_store(qt, 501, q_with_trace, 0);
    pgwt_qt_store(qt, 502, q_no_trace, 0);
    pgwt_qt_close(qt);
    free(qt);

    FILE *f = fopen(jsonl_path(), "r");
    CHECK(f != NULL, "opened query_texts.jsonl");
    char line1[1024] = {0};
    char line2[1024] = {0};
    CHECK(fgets(line1, sizeof(line1), f) != NULL, "read line 1");
    CHECK(fgets(line2, sizeof(line2), f) != NULL, "read line 2");
    fclose(f);

    CHECK(strstr(line1, "\"trace_id\":\"60b6417fa7216a6552a4be6058e5ec9b\"") != NULL,
          "line 1 contains trace_id");
    CHECK(strstr(line1, "\"parent_span_id\":\"325b138ff40c1d29\"") != NULL,
          "line 1 contains parent_span_id");
    CHECK(strstr(line2, "trace_id") == NULL,
          "line 2 has no trace_id");
}

int main(void)
{
    printf("=== test_query_text (T5: DUR-4/10 + F-C1: traceparent) ===\n");
    test_traceparent_parsing();
    test_traceparent_jsonl_output();
    test_append_and_dedup();
    test_torn_line_tolerated();
    test_compaction();
    test_cap_logged();
    test_permissions();
    rm_rf(TEST_DIR);
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}

