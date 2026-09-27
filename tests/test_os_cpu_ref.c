/* test_os_cpu_ref.c — issue #115 Part 2: the "operating system" CPU
 * reference (pgwt_tm_result.os_cpu_ms / has_os_cpu_ref).
 *
 * os_cpu_ms sums measured cpu_ns across the WHOLE window regardless of
 * which PostgreSQL wait/idle event was set (CPU* + on-CPU-while-waiting +
 * on-CPU-while-idle) — the most complete on-CPU figure available WITHOUT a
 * new measurement (compute.c pgwt_compute_time_model). has_os_cpu_ref must
 * be false (reference WITHHELD) whenever any wait/idle interval's on-CPU
 * contribution is genuinely unknown, rather than silently treating it as
 * zero — this file is the bypass suite for that gate: a pure sampled/v2
 * window, and a tier switch mid-window, must both come back false rather
 * than a confident (and wrong) number.
 *
 * Pure: links compute.c directly (-DPGWT_SERVER, no daemon/BPF/PostgreSQL),
 * same pattern as test_live_accum.c. */
#include "compute.h"
#include "pg_wait_tracer.h"
#include "wait_event.h"
#include "summary_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL(%d): " fmt "\n", __LINE__, ##__VA_ARGS__); } \
} while (0)

#define MS(x) ((uint64_t)(x) * 1000000ULL)
#define LWLOCK_EV  WEI(PG_WAIT_LWLOCK, 3)
#define CLIENT_READ PG_WAIT_CLIENT_READ

/* compute.c's only foreign symbol (summary streaming) — unused by the
 * function under test. Same stub as test_live_accum.c. */
int pgwt_visit_summaries(const char *trace_dir, uint64_t from_wall_ns,
                         uint64_t to_wall_ns, pgwt_summary_visitor visitor,
                         void *ctx)
{
    (void)trace_dir; (void)from_wall_ns; (void)to_wall_ns; (void)visitor; (void)ctx;
    return -1;
}

static struct pgwt_trace_event mkev(uint32_t old_event, uint64_t dur_ns,
                                    uint64_t cpu_ns)
{
    struct pgwt_trace_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.timestamp_ns = dur_ns;
    ev.pid = 100;
    ev.old_event = old_event;
    ev.new_event = old_event;
    ev.duration_ns = dur_ns;
    ev.cpu_ns = cpu_ns;
    return ev;
}

/* 1. Full exact-tier coverage: a CPU-class (we==0) interval, a wait interval
 * with measured spin, and an idle interval with measured protocol-tail CPU.
 * All three carry a real cpu_ns, so the reference must be present and equal
 * to the exact sum of all three. */
static void test_full_coverage(void)
{
    struct pgwt_trace_event evs[3] = {
        mkev(0,           MS(100), MS(90)),   /* CPU*: 90ms on-CPU of 100ms gap */
        mkev(LWLOCK_EV,   MS(50),  MS(2)),     /* wait, 2ms measured spin */
        mkev(CLIENT_READ, MS(200), MS(1)),     /* idle, 1ms protocol tail */
    };
    struct pgwt_filter f; memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(evs, 3, &f, 350.0, &tm);

    CHECK(tm.has_os_cpu_ref, "full coverage: reference must be present");
    CHECK(tm.os_cpu_ms > 92.9 && tm.os_cpu_ms < 93.1,
          "os_cpu_ms = 90+2+1 = 93, got %.3f", tm.os_cpu_ms);
    free(tm.rows);
}

/* 2. Bypass case A — pure sampled/v2 window: every cpu_ns is UNKNOWN. The
 * existing has_measured_cpu gate already covers this, but this test pins
 * that the NEW os_cpu_ref field inherits it rather than defaulting to some
 * other, wrongly-permissive rule. RED without the has_measured_cpu &&
 * check: os_cpu_ms would still be a real (if inference-based) number and a
 * naive "os_cpu_ms > 0 => show it" client-side rule would render it. */
static void test_bypass_sampled_no_measurement(void)
{
    struct pgwt_trace_event evs[2] = {
        mkev(0,         MS(100), PGWT_CPU_NS_UNKNOWN),
        mkev(LWLOCK_EV, MS(50),  PGWT_CPU_NS_UNKNOWN),
    };
    struct pgwt_filter f; memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(evs, 2, &f, 150.0, &tm);

    CHECK(!tm.has_os_cpu_ref,
          "sampled/v2 window (no cpu_ns anywhere): reference must be absent");
    free(tm.rows);
}

/* 3. Bypass case B — tier switch mid-window: the CPU-class and one wait
 * interval are measured (exact tier), but a SECOND wait interval closed
 * before escalation finished and carries PGWT_CPU_NS_UNKNOWN. This interval
 * has nonzero wall time, so its on-CPU contribution is truly unknown — not
 * zero. Demonstrated red: without the os_ref_gap tracking, has_measured_cpu
 * alone would be true (the OTHER events are measured) and os_cpu_ms would
 * silently omit the unknown interval's spin as if it were exactly 0. */
static void test_bypass_tier_switch_mid_window(void)
{
    struct pgwt_trace_event evs[3] = {
        mkev(0,         MS(100), MS(80)),              /* measured */
        mkev(LWLOCK_EV, MS(20),  MS(1)),                /* measured */
        mkev(LWLOCK_EV, MS(30),  PGWT_CPU_NS_UNKNOWN),  /* pre-escalation: unknown */
    };
    struct pgwt_filter f; memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(evs, 3, &f, 150.0, &tm);

    CHECK(!tm.has_os_cpu_ref,
          "tier switch mid-window (one wait interval unmeasured): "
          "reference must be withheld, not silently low");
    free(tm.rows);
}

/* 3b. Same bypass, but the unmeasured gap is on an IDLE-labeled interval
 * instead of a wait interval — the idle branch has its own os_ref_gap set
 * site and needs its own red demonstration. */
static void test_bypass_tier_switch_idle_unknown(void)
{
    struct pgwt_trace_event evs[3] = {
        mkev(0,           MS(100), MS(80)),
        mkev(LWLOCK_EV,   MS(20),  MS(1)),
        mkev(CLIENT_READ, MS(30),  PGWT_CPU_NS_UNKNOWN),
    };
    struct pgwt_filter f; memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(evs, 3, &f, 150.0, &tm);

    CHECK(!tm.has_os_cpu_ref,
          "tier switch mid-window (idle interval unmeasured): "
          "reference must be withheld");
    free(tm.rows);
}

/* 4. Zero-that-means-measured, not zero-that-means-absent: an all-idle
 * window where every idle interval genuinely measured 0 on-CPU ns (a truly
 * idle backend). has_os_cpu_ref must be TRUE (it is measured — the flag is
 * what distinguishes this from case 2/3, not the value) with os_cpu_ms == 0.
 * A consumer that inferred "absent" from os_cpu_ms === 0 would wrongly hide
 * a legitimate, trustworthy zero. */
static void test_legitimate_zero_is_not_absent(void)
{
    struct pgwt_trace_event evs[1] = {
        mkev(CLIENT_READ, MS(500), 0),   /* measured: truly 0 on-CPU */
    };
    struct pgwt_filter f; memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(evs, 1, &f, 500.0, &tm);

    CHECK(tm.has_os_cpu_ref,
          "all-idle window with a MEASURED (zero) cpu_ns: reference present");
    CHECK(tm.os_cpu_ms == 0.0, "os_cpu_ms is a real 0, got %.3f", tm.os_cpu_ms);
    free(tm.rows);
}

/* 5. Empty window: no events at all — "the measurement missing for a
 * window" bypass case. Must come back absent, not a stale/garbage true. */
static void test_empty_window_absent(void)
{
    struct pgwt_filter f; memset(&f, 0, sizeof(f));
    struct pgwt_tm_result tm;
    pgwt_compute_time_model(NULL, 0, &f, 1000.0, &tm);

    CHECK(!tm.has_os_cpu_ref, "empty window: reference must be absent");
    free(tm.rows);
}

int main(void)
{
    test_full_coverage();
    test_bypass_sampled_no_measurement();
    test_bypass_tier_switch_mid_window();
    test_bypass_tier_switch_idle_unknown();
    test_legitimate_zero_is_not_absent();
    test_empty_window_absent();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
