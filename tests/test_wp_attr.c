/* #390: structurally pin the production attributes, not measured event loss.
 * No privileges, BPF program, or PostgreSQL required. */
#include "perf_event.h"

#include <linux/hw_breakpoint.h>
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; printf("  PASS: " fmt "\n", ##__VA_ARGS__); } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while (0)

static void test_attr(int start_enabled)
{
    const uint64_t addr = UINT64_C(0x123456789abc);
    struct perf_event_attr attr;
    memset(&attr, 0xa5, sizeof(attr));
    pgwt_fill_watchpoint_attr(&attr, addr, start_enabled);

    printf("--- watchpoint attributes (start_enabled=%d) ---\n", start_enabled);
    CHECK(attr.sample_period == 1,
          "issue #390: sample_period == 1; a non-1 period makes the watchpoint throttleable");
    CHECK(attr.freq == 0,
          "issue #390: freq == 0; freq=1 makes the watchpoint throttleable");
    CHECK(attr.type == PERF_TYPE_BREAKPOINT, "type == PERF_TYPE_BREAKPOINT");
    CHECK(attr.bp_type == HW_BREAKPOINT_W, "bp_type == HW_BREAKPOINT_W");
    CHECK(attr.bp_len == HW_BREAKPOINT_LEN_4, "bp_len == HW_BREAKPOINT_LEN_4");
    CHECK(attr.bp_addr == addr, "bp_addr == supplied address");
    CHECK(attr.sample_type == 0, "sample_type == 0");
    CHECK(attr.wakeup_events == 0, "wakeup_events == 0");
    CHECK(attr.disabled == (start_enabled ? 0 : 1),
          "disabled matches start_enabled=%d", start_enabled);
    CHECK(attr.size == sizeof(struct perf_event_attr),
          "size == sizeof(struct perf_event_attr)");
}

int main(void)
{
    test_attr(0);
    test_attr(1);
    printf("\n%d/%d checks passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
