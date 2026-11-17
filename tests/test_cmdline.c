/* test_cmdline.c — Unit tests for cmdline parser (backend type names) */
#include "cmdline.h"

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

static void test_backend_type_names(void)
{
    printf("--- Backend Type Names ---\n");

    struct { enum pgwt_backend_type bt; const char *name; } cases[] = {
        { PGWT_BT_CLIENT,           "client" },
        { PGWT_BT_CHECKPOINTER,     "checkpointer" },
        { PGWT_BT_BG_WRITER,        "bgwriter" },
        { PGWT_BT_WAL_WRITER,       "walwriter" },
        { PGWT_BT_AUTOVAC_LAUNCHER, "autovac_launcher" },
        { PGWT_BT_AUTOVAC_WORKER,   "autovac_worker" },
        { PGWT_BT_WAL_SENDER,       "walsender" },
        { PGWT_BT_WAL_RECEIVER,     "walreceiver" },
        { PGWT_BT_STARTUP,          "startup" },
        { PGWT_BT_LOGICAL_LAUNCHER, "logical_launcher" },
        { PGWT_BT_LOGICAL_WORKER,   "logical_worker" },
        { PGWT_BT_ARCHIVER,         "archiver" },
        { PGWT_BT_LOGGER,           "logger" },
        { PGWT_BT_PARALLEL_WORKER,  "parallel_worker" },
        { PGWT_BT_IO_WORKER,        "io_worker" },
        { PGWT_BT_BG_WORKER,       "bg_worker" },
        { PGWT_BT_UNKNOWN,          "unknown" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *got = pgwt_backend_type_name(cases[i].bt);
        CHECK(strcmp(got, cases[i].name) == 0,
              "bt=%d: expected \"%s\", got \"%s\"",
              cases[i].bt, cases[i].name, got);
    }
}

static void test_enum_coverage(void)
{
    printf("--- Enum Coverage ---\n");
    /* PGWT_BT_UNKNOWN should be the last enum value */
    CHECK(PGWT_BT_UNKNOWN == 16,
          "PGWT_BT_UNKNOWN should be 16, got %d", PGWT_BT_UNKNOWN);
    /* All values 0..PGWT_BT_UNKNOWN should return non-NULL */
    for (int i = 0; i <= PGWT_BT_UNKNOWN; i++) {
        const char *name = pgwt_backend_type_name((enum pgwt_backend_type)i);
        CHECK(name != NULL && strlen(name) > 0,
              "bt=%d returned NULL or empty", i);
    }
    /* Out of range should return "unknown" */
    const char *oob = pgwt_backend_type_name((enum pgwt_backend_type)99);
    CHECK(strcmp(oob, "unknown") == 0,
          "out-of-range bt=99 should return \"unknown\", got \"%s\"", oob);
}

static void test_no_command_proof(void)
{
    printf("--- Pinned stat exit proof (ported from escalation fixtures) ---\n");
    struct { char state; int parent; unsigned flags, vsize; int expected; } cases[] = {
        {'R', 7, 0, 8192, 0}, {'S', 7, 4, 0, 2},
        {'R', 7, 4, 8192, 0}, {'R', 7, 0, 0, 0},
        {'Z', 7, 0, 0, 1}, {'X', 7, 0, 0, 1}, {'x', 7, 0, 0, 1},
        {'R', 8, 4, 0, -1}, {'?', 7, 4, 0, -1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        FILE *f = tmpfile();
        CHECK(f != NULL, "stat fixture opened");
        if (!f) continue;
        fprintf(f, "42 (backend (worker)) %c %d 0 0 0 0 %u 0 0 0 0 0 0 0 -5 0 0 0 0 123 %u",
                cases[i].state, cases[i].parent, cases[i].flags, cases[i].vsize);
        fflush(f);
        CHECK(pgwt_backend_no_command_fd(fileno(f), 7) == cases[i].expected,
              "stat proof case %zu", i);
        fclose(f);
    }
    const char *bad[] = {"", "broken stat", "42 (backend) R 7",
        "42 (backend) R 7 0 0 0 0 -4 0 0 0 0 0 0 0 0 0 0 0 0 123 0",
        "42 (backend) R 7 0 0 0 0 18446744073709551616 0 0 0 0 0 0 0 0 0 0 0 0 123 0"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        FILE *f = tmpfile();
        CHECK(f != NULL, "invalid stat fixture opened");
        if (!f) continue;
        fputs(bad[i], f);
        fflush(f);
        CHECK(pgwt_backend_no_command_fd(fileno(f), 7) == -1,
              "invalid stat proof refused %zu", i);
        fclose(f);
    }
    FILE *f = tmpfile();
    CHECK(f != NULL, "truncated stat fixture opened");
    if (f) {
        for (int i = 0; i < 1100; ++i) fputc('x', f);
        fflush(f);
        CHECK(pgwt_backend_no_command_fd(fileno(f), 7) == -1, "truncated stat refused");
        fclose(f);
    }
    CHECK(pgwt_backend_no_command_fd(-ENOENT, 7) == 1, "missing pinned target");
    CHECK(pgwt_backend_no_command_fd(-ESRCH, 7) == 1, "exited pinned target");
    CHECK(pgwt_backend_no_command_fd(-EACCES, 7) == -1, "unknown target refused");
    CHECK(pgwt_backend_no_command_fd(-EIO, 7) == -1, "stat I/O error refused");
}

int main(void)
{
    printf("=== test_cmdline ===\n");
    test_backend_type_names();
    test_enum_coverage();
    test_no_command_proof();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
