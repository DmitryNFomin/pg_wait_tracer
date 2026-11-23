/* test_timeline_summary.c — timeline per-event / per-query summary (#1006).
 *
 * S0: pins the summary row structs and checks the stub refuses. S2 adds:
 * aggregation rules (ZERO_MARKER never summed, TRUNC_* durations), the
 * re-aggregation equality (live adds == pgwt_tl_summary_from_dir of the
 * written segments), JSON and text output, event names.
 *
 * Links timeline_summary.c, timeline_writer.c (the reader) and the event
 * name table (wait_event.c with its idle_rule/spawn/cJSON closure).
 * Built on the Mac by scripts/check.sh (host C, ASan/UBSan) and on Linux by
 * tests/Makefile; listed in tests/unit_tests.list.
 */
#include "../src/timeline_summary.h"
#include "../src/timeline_writer.h"
#include "../src/wait_event.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); fails++; } } while (0)

static void test_row_layout(void)
{
    printf("=== 1. summary row layout ===\n");
    CHECK(sizeof(struct pgwt_tl_event_row) == 48);
    CHECK(sizeof(struct pgwt_tl_query_row) == 40);
    CHECK(sizeof(struct pgwt_tl_summary_totals) == 64);
    CHECK(sizeof(struct pgwt_tl_record) == PGWT_TL_REC_SIZE);
}

static void test_event_names_link(void)
{
    printf("=== 2. event names resolve through the linked table ===\n");
    char buf[128];
    pgwt_init_event_names(17);
    pgwt_event_full_name(0x0A000000u, buf, sizeof(buf));
    CHECK(strncmp(buf, "IO:", 3) == 0);
}

static void test_stub_refuses(void)
{
    printf("=== 3. S0 stub refuses (never an empty success) ===\n");
    errno = 0;
    CHECK(pgwt_tl_summary_create() == NULL);
    struct pgwt_tl_record rec;
    memset(&rec, 0, sizeof(rec));
    CHECK(pgwt_tl_summary_add(NULL, &rec) == -ENOSYS);
    CHECK(pgwt_tl_summary_from_dir(NULL, "/nonexistent") == -ENOSYS);
    char why[64] = "";
    CHECK(pgwt_tl_summary_equal(NULL, NULL, why, sizeof(why)) == 0);
    CHECK(why[0] != '\0');
}

int main(void)
{
    test_row_layout();
    test_event_names_link();
    test_stub_refuses();
    if (fails) {
        fprintf(stderr, "test_timeline_summary: %d check(s) failed\n", fails);
        return 1;
    }
    printf("test_timeline_summary: all checks passed\n");
    return 0;
}
