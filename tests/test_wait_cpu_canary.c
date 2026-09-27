/* test_wait_cpu_canary.c — the offline wait-CPU canary's decision logic.
 *
 * Compiles wait_cpu_canary.c with -DWCC_NO_MAIN so the pure aggregation and
 * verdict functions are unit-testable without a trace file.
 *
 * The canary's job is to notice CPU recorded inside an interval the backend
 * was supposed to be asleep for (issue #202's over-attribution direction).
 * Its detection is only worth having if it cannot be satisfied by NOT
 * LOOKING, so most of this file is the ways it could go blind:
 *
 *   - no events at all;
 *   - events, but not one carrying a measured cpu_ns (v2 file, sampled
 *     tier, no BTF) — the silent 0/0 = "0.0%, clean" trap;
 *   - a cpu_ns of PGWT_CPU_NS_UNKNOWN folded in as if it were 0;
 *   - the flagged class being EMPTY rather than clean;
 *   - a defect in a class the caller never iterates.
 *
 * Pure: no trace, no BPF, no PostgreSQL. */
#include "wait_cpu_canary.c"

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
#define US(x) ((uint64_t)(x) * 1000ULL)

static void zero(struct wcc_stats *by_class)
{
    memset(by_class, 0, sizeof(struct wcc_stats) * PGWT_NUM_CLASSES);
}

/* ── 1. Which classes the canary is allowed to flag ─────────────────── */
static void test_pure_sleep_classes(void)
{
    printf("--- pure-sleep classes ---\n");
    CHECK(wcc_is_pure_sleep_class(PGWT_CLASS_LOCK), "Lock is a pure sleep");
    CHECK(wcc_is_pure_sleep_class(PGWT_CLASS_TIMEOUT),
          "Timeout is a pure sleep");
    /* The one the coordinator warned about: IO legitimately carries the
     * syscall's own on-CPU work and must NEVER be flagged. */
    CHECK(!wcc_is_pure_sleep_class(PGWT_CLASS_IO),
          "IO is NOT flagged — it legitimately carries CPU");
    CHECK(!wcc_is_pure_sleep_class(PGWT_CLASS_LWLOCK),
          "LWLock is not flagged (it spins before blocking)");
    CHECK(!wcc_is_pure_sleep_class(PGWT_CLASS_CPU), "CPU is not a wait");
}

/* ── 2. The signature it exists to catch ────────────────────────────── */
static void test_detects_sleep_cpu(void)
{
    printf("--- detection: CPU inside a pure sleep ---\n");
    struct wcc_stats by_class[PGWT_NUM_CLASSES];

    /* A healthy pg_sleep: 10 x 1 s of Timeout, ~20 us of wakeup work each.
     * 0.002% — two orders under the 0.1% limit. */
    zero(by_class);
    for (int i = 0; i < 10; i++)
        wcc_add(&by_class[PGWT_CLASS_TIMEOUT], MS(1000), US(20));
    CHECK(wcc_flags(&by_class[PGWT_CLASS_TIMEOUT], PGWT_CLASS_TIMEOUT) == 0,
          "20 us of wakeup work per 1 s sleep is clean (%.4f%%)",
          wcc_ratio_pct(&by_class[PGWT_CLASS_TIMEOUT]));
    CHECK(wcc_verdict(by_class, 10, 10) == WCC_CLEAN, "…verdict CLEAN");

    /* The signature: ONE event carrying 3 ms of CPU inside a sleep. The
     * class ratio stays tiny (0.03%), so the per-event limit is what has to
     * catch it — a ratio-only canary would miss this. */
    zero(by_class);
    for (int i = 0; i < 9; i++)
        wcc_add(&by_class[PGWT_CLASS_TIMEOUT], MS(1000), US(20));
    wcc_add(&by_class[PGWT_CLASS_TIMEOUT], MS(1000), MS(3));
    CHECK(wcc_ratio_pct(&by_class[PGWT_CLASS_TIMEOUT]) < WCC_RATIO_LIMIT_PCT,
          "…the class ratio alone stays under the limit (%.4f%%)",
          wcc_ratio_pct(&by_class[PGWT_CLASS_TIMEOUT]));
    CHECK(wcc_flags(&by_class[PGWT_CLASS_TIMEOUT], PGWT_CLASS_TIMEOUT)
          == WCC_FLAG_EVENT,
          "…but the 3 ms single event is flagged (flags=%d)",
          wcc_flags(&by_class[PGWT_CLASS_TIMEOUT], PGWT_CLASS_TIMEOUT));
    CHECK(wcc_verdict(by_class, 10, 10) == WCC_DEFECT, "…verdict DEFECT");

    /* Sustained low-level leakage: every event under 1 ms, but 1% of the
     * wait is on-CPU. The per-event limit misses it; the ratio catches it. */
    zero(by_class);
    for (int i = 0; i < 100; i++)
        wcc_add(&by_class[PGWT_CLASS_LOCK], MS(50), US(500));
    CHECK(by_class[PGWT_CLASS_LOCK].max_cpu_ns < WCC_EVENT_LIMIT_NS,
          "…no single event reaches 1 ms (max %llu ns)",
          (unsigned long long)by_class[PGWT_CLASS_LOCK].max_cpu_ns);
    CHECK(wcc_flags(&by_class[PGWT_CLASS_LOCK], PGWT_CLASS_LOCK)
          == WCC_FLAG_RATIO,
          "…the 1.0%% class ratio is flagged (%.4f%%)",
          wcc_ratio_pct(&by_class[PGWT_CLASS_LOCK]));
    CHECK(wcc_verdict(by_class, 100, 100) == WCC_DEFECT, "…verdict DEFECT");

    /* IO with the SAME shape must stay clean — the false-positive the
     * coordinator called out: a pwrite into page cache is nearly all
     * on-CPU under an IO label. */
    zero(by_class);
    for (int i = 0; i < 100; i++)
        wcc_add(&by_class[PGWT_CLASS_IO], MS(50), MS(45));
    CHECK(wcc_flags(&by_class[PGWT_CLASS_IO], PGWT_CLASS_IO) == 0,
          "90%% cpu/dur under an IO label is clean, not a defect (%.1f%%)",
          wcc_ratio_pct(&by_class[PGWT_CLASS_IO]));
    CHECK(wcc_verdict(by_class, 100, 100) == WCC_CLEAN, "…verdict CLEAN");
}

/* ── 3. cpu_ns > duration_ns: impossible in any class ───────────────── */
static void test_cpu_over_duration(void)
{
    printf("--- cpu_ns > duration_ns ---\n");
    struct wcc_stats by_class[PGWT_NUM_CLASSES];

    /* src/compute.c:810's wait branch adds cpu_ns to wait_gap_cpu_ns with
     * no clamp (only the CPU-gap branch clamps), so cpu_clamped_ms == 0
     * says nothing about wait events. This is the only check in the tree
     * that looks. */
    zero(by_class);
    wcc_add(&by_class[PGWT_CLASS_IO], MS(10), MS(12));
    CHECK(by_class[PGWT_CLASS_IO].n_cpu_gt_dur == 1,
          "an IO event with cpu 12 ms over a 10 ms interval is counted");
    CHECK(by_class[PGWT_CLASS_IO].max_excess_ns == MS(2),
          "…excess recorded as 2 ms (got %llu)",
          (unsigned long long)by_class[PGWT_CLASS_IO].max_excess_ns);
    CHECK(wcc_flags(&by_class[PGWT_CLASS_IO], PGWT_CLASS_IO)
          == WCC_FLAG_OVER_DUR,
          "…and flagged even though IO is exempt from the sleep limits");
    CHECK(wcc_verdict(by_class, 1, 1) == WCC_DEFECT, "…verdict DEFECT");

    /* Exactly equal is legal: a task can be on-CPU for the whole interval. */
    zero(by_class);
    wcc_add(&by_class[PGWT_CLASS_IO], MS(10), MS(10));
    CHECK(by_class[PGWT_CLASS_IO].n_cpu_gt_dur == 0 &&
          wcc_flags(&by_class[PGWT_CLASS_IO], PGWT_CLASS_IO) == 0,
          "cpu_ns == duration_ns is legal, not a defect");
}

/* ── 4. The bypass suite: a canary that cannot see must REFUSE ──────── */
static void test_cannot_see_refuses(void)
{
    printf("--- blindness must refuse, never approve ---\n");
    struct wcc_stats by_class[PGWT_NUM_CLASSES];

    /* (a) Empty input. Every ratio is 0.0%, every max is 0 — the shape of a
     * perfectly clean run. It must NOT be reported as clean. */
    zero(by_class);
    CHECK(wcc_verdict(by_class, 0, 0) == WCC_CANNOT_SEE,
          "no events at all -> CANNOT CHECK, not CLEAN");

    /* (b) Events, but nothing measured (v2 trace, sampled tier, no BTF).
     * This is the trap the whole canary turns on: the sums are 0 because
     * nothing was looked at, which reads identically to 0 because nothing
     * was wrong. */
    zero(by_class);
    for (int i = 0; i < 50; i++)
        wcc_add(&by_class[PGWT_CLASS_TIMEOUT], MS(1000),
                PGWT_CPU_NS_UNKNOWN);
    CHECK(by_class[PGWT_CLASS_TIMEOUT].n_events == 50,
          "…the 50 unmeasured events are still counted as events");
    CHECK(by_class[PGWT_CLASS_TIMEOUT].n_measured == 0,
          "…none of them counts as measured");
    CHECK(by_class[PGWT_CLASS_TIMEOUT].sum_cpu_ns == 0 &&
          by_class[PGWT_CLASS_TIMEOUT].max_cpu_ns == 0,
          "…UNKNOWN is never folded in as a cpu_ns of 0 (sum %llu, max %llu)",
          (unsigned long long)by_class[PGWT_CLASS_TIMEOUT].sum_cpu_ns,
          (unsigned long long)by_class[PGWT_CLASS_TIMEOUT].max_cpu_ns);
    CHECK(wcc_flags(&by_class[PGWT_CLASS_TIMEOUT], PGWT_CLASS_TIMEOUT) == 0,
          "…an all-unmeasured class is not flagged (there is no evidence)");
    CHECK(wcc_verdict(by_class, 50, 0) == WCC_CANNOT_SEE,
          "…and the RUN refuses: 50 events, 0 measured -> CANNOT CHECK");

    /* (c) The flagged classes are absent rather than clean, while other
     * classes were measured. There is genuinely nothing to say about
     * pure sleeps, and the run is clean on the evidence it does have —
     * but the per-class rows must show 0 events, not 0.0% of something. */
    zero(by_class);
    for (int i = 0; i < 20; i++)
        wcc_add(&by_class[PGWT_CLASS_IO], MS(5), MS(4));
    CHECK(by_class[PGWT_CLASS_TIMEOUT].n_events == 0 &&
          by_class[PGWT_CLASS_LOCK].n_events == 0,
          "…no pure-sleep events present");
    CHECK(wcc_verdict(by_class, 20, 20) == WCC_CLEAN,
          "…verdict CLEAN on the measured IO evidence");

    /* (d) A single measured event among many unmeasured ones is enough to
     * see with — and enough to fail on. Blindness is about having NO
     * evidence, not about having little. */
    zero(by_class);
    for (int i = 0; i < 99; i++)
        wcc_add(&by_class[PGWT_CLASS_LOCK], MS(100), PGWT_CPU_NS_UNKNOWN);
    wcc_add(&by_class[PGWT_CLASS_LOCK], MS(100), MS(4));
    CHECK(wcc_verdict(by_class, 100, 1) == WCC_DEFECT,
          "1 measured event out of 100 still fails on a 4 ms sleep-CPU");

    /* (e) The verdict must scan EVERY class, not just the first or the
     * pure-sleep ones — a cpu>dur in the last class must still fail. */
    zero(by_class);
    wcc_add(&by_class[PGWT_CLASS_CPU], MS(10), MS(9));
    wcc_add(&by_class[PGWT_CLASS_UNKNOWN], MS(3), MS(7));
    CHECK(wcc_verdict(by_class, 2, 2) == WCC_DEFECT,
          "a defect in the LAST class index is still found");

    /* (f) Degenerate arithmetic: a zero-duration interval must not divide
     * by zero or report a ratio at all. */
    zero(by_class);
    wcc_add(&by_class[PGWT_CLASS_TIMEOUT], 0, 0);
    CHECK(wcc_ratio_pct(&by_class[PGWT_CLASS_TIMEOUT]) == 0.0,
          "a zero-duration interval gives a 0%% ratio, not a NaN");
    CHECK(wcc_flags(&by_class[PGWT_CLASS_TIMEOUT], PGWT_CLASS_TIMEOUT) == 0,
          "…and is not flagged");
}

int main(void)
{
    test_pure_sleep_classes();
    test_detects_sleep_cpu();
    test_cpu_over_duration();
    test_cannot_see_refuses();
    printf("\n%d/%d checks passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
