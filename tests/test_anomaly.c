/* test_anomaly.c — Unit tests for the anomaly rule engine (Phase A5)
 *
 * Drives the PURE rule core (pgwt_anomaly_eval / pgwt_anomaly_metrics_from_batch)
 * with scripted sample-stream inputs and asserts fire / no-fire across:
 *   - AAS-vs-baseline (factor, sustained N ticks, baseline warmup)
 *   - lock-class fraction (threshold, sustained N ticks)
 *   - hysteresis / cooldown (a flapping metric cannot re-fire inside cooldown)
 *   - budget-blocked-silent (modeled at the daemon layer — here we assert the
 *     pure core still FIREs, since the budget lives in the escalation engine)
 *
 * Built with -DPGWT_SERVER against anomaly.c so only the BPF-free core is
 * compiled (no skeleton, no escalation engine) — runnable in CI's server-only
 * jobs, matching test_sampler / test_trace_v2.
 */
#define _GNU_SOURCE
#include "anomaly.h"
#include "pg_wait_tracer.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, fmt, ...) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

/* One tick in ns at 10 Hz. */
#define TICK_NS 100000000ULL

/* Feed the engine a constant (aas, lock_fraction) for `ticks`, advancing the
 * monotonic clock one tick per call. Returns the LAST decision; *fires
 * (if non-NULL) accumulates the number of FIRE decisions seen. */
static struct pgwt_anomaly_decision
feed(struct pgwt_anomaly *a, double aas, double lock_frac, int ticks,
     uint64_t *clock, int *fires)
{
    struct pgwt_anomaly_decision d;
    memset(&d, 0, sizeof(d));
    for (int i = 0; i < ticks; i++) {
        d = pgwt_anomaly_eval(a, aas, lock_frac, *clock);
        if (fires && d.action == PGWT_ANOMALY_FIRE)
            (*fires)++;
        *clock += TICK_NS;
    }
    return d;
}

/* Warm the baseline to ~level by feeding `level` AAS for enough normal ticks. */
static void warm_baseline(struct pgwt_anomaly *a, double level,
                          uint64_t *clock)
{
    feed(a, level, 0.0, a->warmup_needed + 5, clock, NULL);
}

/* ── Test 1: disabled engine never fires ──────────────────────────────── */
static void test_disabled(void)
{
    printf("--- disabled engine: never fires ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, false, 10);
    uint64_t clk = TICK_NS;
    int fires = 0;
    struct pgwt_anomaly_decision d =
        feed(&a, 1000.0, 0.99, 50, &clk, &fires);
    CHECK(d.action == PGWT_ANOMALY_NONE, "disabled produced action %d",
          d.action);
    CHECK(fires == 0, "disabled fired %d times", fires);
}

/* ── Test 2: AAS-vs-baseline fires only after sustained N ticks ────────── */
static void test_aas_sustained(void)
{
    printf("--- AAS rule: fire after sustained N ticks ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_factor = 3.0;
    a.aas_ticks  = 3;
    uint64_t clk = TICK_NS;

    warm_baseline(&a, 2.0, &clk);   /* baseline ~2 active sessions */
    CHECK(a.baseline_aas > 1.0 && a.baseline_aas < 3.0,
          "baseline=%.2f expected ~2", a.baseline_aas);

    /* Spike to 10 (> 3*2=6). First two ticks: NEAR (sustain not met). */
    struct pgwt_anomaly_decision d1 = pgwt_anomaly_eval(&a, 10.0, 0.0, clk);
    clk += TICK_NS;
    CHECK(d1.action == PGWT_ANOMALY_NEAR &&
          (d1.near_mask & PGWT_NEAR_AAS_SUSTAIN),
          "tick1 expected NEAR(aas-sustain), got action=%d mask=%u",
          d1.action, d1.near_mask);

    struct pgwt_anomaly_decision d2 = pgwt_anomaly_eval(&a, 10.0, 0.0, clk);
    clk += TICK_NS;
    CHECK(d2.action == PGWT_ANOMALY_NEAR, "tick2 expected NEAR, got %d",
          d2.action);

    /* Third sustained tick: FIRE. */
    struct pgwt_anomaly_decision d3 = pgwt_anomaly_eval(&a, 10.0, 0.0, clk);
    clk += TICK_NS;
    CHECK(d3.action == PGWT_ANOMALY_FIRE &&
          (d3.fired_mask & PGWT_RULE_AAS),
          "tick3 expected FIRE(aas), got action=%d mask=%u",
          d3.action, d3.fired_mask);
}

/* ── Test 3: AAS within factor never fires ─────────────────────────────── */
static void test_aas_no_fire(void)
{
    printf("--- primary AAS rule: under factor never fires ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_factor = 3.0;
    /* This pre-existing case isolates the primary rule. Under the new default
     * secondary rule, 4 -> 8 is correctly considered a meaningful doubling. */
    a.aas_secondary_factor = 0.0;
    a.aas_ticks  = 3;
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 4.0, &clk);   /* baseline ~4 */

    /* AAS = 8 < 3*4 = 12 → never fires even sustained for many ticks. */
    int fires = 0;
    feed(&a, 8.0, 0.0, 50, &clk, &fires);
    CHECK(fires == 0, "AAS under factor fired %d times", fires);
}

/* ── AAS-1: additive/secondary trigger and false-positive guards ───── */
static void test_busy_baseline_secondary_fires(void)
{
    printf("--- AAS-1: busy baseline 2 -> 4 fires via secondary rule ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_ticks = 3;
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 2.0, &clk);

    CHECK(a.aas_abs_floor == 2.0, "default abs floor %.2f expected 2.0",
          a.aas_abs_floor);
    CHECK(a.aas_abs_delta == 1.5, "default abs delta %.2f expected 1.5",
          a.aas_abs_delta);
    CHECK(a.aas_secondary_factor == 1.5,
          "default secondary factor %.2f expected 1.5",
          a.aas_secondary_factor);
    CHECK(4.0 <= a.aas_factor * a.baseline_aas,
          "regression pin must not clear primary threshold %.2f",
          a.aas_factor * a.baseline_aas);

    struct pgwt_anomaly_decision d =
        feed(&a, 4.0, 0.0, a.aas_ticks, &clk, NULL);
    CHECK(d.action == PGWT_ANOMALY_FIRE
          && (d.fired_mask & PGWT_RULE_AAS),
          "busy-baseline spike expected FIRE(aas), got action=%d mask=%u",
          d.action, d.fired_mask);
}

static void test_idle_baseline_primary_still_fires(void)
{
    printf("--- AAS-1: idle baseline 1 -> 4 still fires via primary rule ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_ticks = 3;
    /* Isolate the unchanged strict-primary behavior. */
    a.aas_secondary_factor = 0.0;
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 1.0, &clk);

    CHECK(4.0 > a.aas_factor * a.baseline_aas,
          "idle-baseline spike must clear primary threshold %.2f",
          a.aas_factor * a.baseline_aas);
    struct pgwt_anomaly_decision d =
        feed(&a, 4.0, 0.0, a.aas_ticks, &clk, NULL);
    CHECK(d.action == PGWT_ANOMALY_FIRE
          && (d.fired_mask & PGWT_RULE_AAS),
          "idle-baseline spike expected FIRE(aas), got action=%d mask=%u",
          d.action, d.fired_mask);
}

static void test_busy_baseline_jitter_does_not_fire(void)
{
    printf("--- AAS-1: busy-baseline jitter does not fire ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 2.0, &clk);

    const double jitter[] = {2.0, 2.3, 2.6, 2.2, 2.5, 2.1};
    int fires = 0;
    for (int i = 0; i < 120; i++) {
        struct pgwt_anomaly_decision d =
            pgwt_anomaly_eval(&a, jitter[i % 6], 0.0, clk);
        clk += TICK_NS;
        if (d.action == PGWT_ANOMALY_FIRE)
            fires++;
    }
    CHECK(fires == 0, "busy-baseline jitter fired %d times", fires);
    CHECK(a.aas_over_streak == 0,
          "jitter left AAS over-streak at %d", a.aas_over_streak);
}

static void test_large_baseline_small_jump_does_not_fire(void)
{
    printf("--- AAS-1: large baseline 50 -> 55 does not fire ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 50.0, &clk);

    int fires = 0;
    feed(&a, 55.0, 0.0, 120, &clk, &fires);
    CHECK(fires == 0, "large-baseline small jump fired %d times", fires);
    CHECK(a.aas_over_streak == 0,
          "small relative jump left AAS over-streak at %d",
          a.aas_over_streak);
}

static void test_large_baseline_real_spike_fires(void)
{
    printf("--- AAS-1: large baseline 50 -> 85 fires ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_ticks = 3;
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 50.0, &clk);

    CHECK(85.0 <= a.aas_factor * a.baseline_aas,
          "large spike regression must not clear primary threshold %.2f",
          a.aas_factor * a.baseline_aas);
    struct pgwt_anomaly_decision d =
        feed(&a, 85.0, 0.0, a.aas_ticks, &clk, NULL);
    CHECK(d.action == PGWT_ANOMALY_FIRE
          && (d.fired_mask & PGWT_RULE_AAS),
          "large-baseline real spike expected FIRE, got action=%d mask=%u",
          d.action, d.fired_mask);
}

static void test_aas_factor_disable_contract(void)
{
    printf("--- AAS-1: million-factor disables both AAS paths ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_factor = PGWT_ANOMALY_AAS_DISABLE_FACTOR;
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 2.0, &clk);

    int fires = 0;
    struct pgwt_anomaly_decision d =
        feed(&a, 4.0, 0.0, 20, &clk, &fires);
    CHECK(fires == 0, "disabled AAS paths fired %d times", fires);
    CHECK(d.action == PGWT_ANOMALY_NONE,
          "disabled AAS paths produced action %d", d.action);
    CHECK(a.aas_over_streak == 0,
          "disabled AAS paths left over-streak at %d", a.aas_over_streak);
}

/* ── Test 4: lock-class fraction rule ──────────────────────────────────── */
static void test_lock_fraction(void)
{
    printf("--- lock-fraction rule: fire after sustained N ticks ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.lock_fraction = 0.30;
    a.lock_ticks    = 3;
    /* Disable AAS so this isolates the lock rule. */
    a.aas_factor = 0.0;
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 2.0, &clk);

    /* lock_frac 0.2 < 0.3 → no fire. */
    int fires = 0;
    feed(&a, 5.0, 0.20, 10, &clk, &fires);
    CHECK(fires == 0, "lock_frac under threshold fired %d", fires);

    /* lock_frac 0.6 > 0.3, sustained 3 ticks → fire on the 3rd. */
    struct pgwt_anomaly_decision d1 = pgwt_anomaly_eval(&a, 5.0, 0.6, clk);
    clk += TICK_NS;
    struct pgwt_anomaly_decision d2 = pgwt_anomaly_eval(&a, 5.0, 0.6, clk);
    clk += TICK_NS;
    struct pgwt_anomaly_decision d3 = pgwt_anomaly_eval(&a, 5.0, 0.6, clk);
    clk += TICK_NS;
    CHECK(d1.action == PGWT_ANOMALY_NEAR &&
          (d1.near_mask & PGWT_NEAR_LOCK_SUSTAIN), "lock tick1 not NEAR");
    CHECK(d2.action == PGWT_ANOMALY_NEAR, "lock tick2 not NEAR");
    CHECK(d3.action == PGWT_ANOMALY_FIRE &&
          (d3.fired_mask & PGWT_RULE_LOCK),
          "lock tick3 expected FIRE(lock), got action=%d mask=%u",
          d3.action, d3.fired_mask);
}

/* ── Test 5: cooldown suppresses re-fire (hysteresis) ──────────────────── */
static void test_cooldown(void)
{
    printf("--- cooldown: flapping metric cannot re-fire ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.lock_fraction = 0.30;
    a.lock_ticks    = 1;          /* fire immediately on cross */
    a.aas_factor    = 0.0;        /* isolate the lock rule */
    a.cooldown_ns   = 100ULL * TICK_NS;   /* 10s cooldown @10Hz */
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 2.0, &clk);

    /* First over-threshold tick → FIRE. */
    struct pgwt_anomaly_decision d1 = pgwt_anomaly_eval(&a, 5.0, 0.9, clk);
    clk += TICK_NS;
    CHECK(d1.action == PGWT_ANOMALY_FIRE, "first cross expected FIRE, got %d",
          d1.action);

    /* Immediately over again (flap) but inside cooldown → NEAR(cooldown). */
    int fires_in_cooldown = 0;
    for (int i = 0; i < 50; i++) {   /* 50 ticks = 5s < 10s cooldown */
        struct pgwt_anomaly_decision d =
            pgwt_anomaly_eval(&a, 5.0, 0.9, clk);
        clk += TICK_NS;
        if (d.action == PGWT_ANOMALY_FIRE)
            fires_in_cooldown++;
        if (d.action == PGWT_ANOMALY_NEAR && (d.near_mask & PGWT_NEAR_COOLDOWN))
            CHECK(d.fired_mask & PGWT_RULE_LOCK,
                  "cooldown NEAR should carry the would-fire rule");
    }
    CHECK(fires_in_cooldown == 0, "fired %d times inside cooldown",
          fires_in_cooldown);
    CHECK(a.dropped_cooldown > 0, "no cooldown drops recorded");

    /* After cooldown elapses, a sustained cross fires again. */
    clk += 200ULL * TICK_NS;   /* jump well past cooldown */
    struct pgwt_anomaly_decision d2 = pgwt_anomaly_eval(&a, 5.0, 0.9, clk);
    CHECK(d2.action == PGWT_ANOMALY_FIRE, "post-cooldown expected FIRE, got %d",
          d2.action);
}

/* ── Test 6: baseline does not absorb a sustained anomaly ──────────────── */
static void test_baseline_protected(void)
{
    printf("--- baseline: sustained anomaly must not poison baseline ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_factor = 3.0;
    a.aas_ticks  = 3;
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 2.0, &clk);
    double base_before = a.baseline_aas;

    /* A long incident at AAS=20. The baseline must stay near 2, so the rule
     * keeps firing (well, NEAR after the cooldown gate) instead of the bar
     * creeping up to 20 and silencing the rule. */
    feed(&a, 20.0, 0.0, 300, &clk, NULL);
    CHECK(a.baseline_aas < base_before + 1.0,
          "baseline crept to %.2f (was %.2f) — anomaly poisoned it",
          a.baseline_aas, base_before);
}

/* ── Test 7: budget-drop is silent at the engine boundary ──────────────── */
/* The pure core does NOT know about budget — it always returns FIRE; the
 * daemon wrapper observes pgwt_escalate's silent denial and records
 * dropped_budget. We assert the pure core keeps FIRing (it must, so the wrapper
 * gets a chance to attempt the escalate) and that fires_total counts attempts,
 * which is the number the budget layer then accepts-or-silently-drops. */
static void test_budget_boundary(void)
{
    printf("--- budget: pure core fires; budget handled at wrapper ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_factor  = 3.0;
    a.aas_ticks   = 1;
    a.cooldown_ns = 0;            /* no cooldown so each cross can fire */
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 2.0, &clk);

    int fires = 0;
    /* Cross repeatedly, jumping past any cooldown each time (cooldown=0). */
    for (int i = 0; i < 5; i++) {
        struct pgwt_anomaly_decision d = pgwt_anomaly_eval(&a, 20.0, 0.0, clk);
        clk += TICK_NS;
        if (d.action == PGWT_ANOMALY_FIRE)
            fires++;
    }
    CHECK(fires >= 1, "expected the core to FIRE on a sustained spike");
    CHECK(a.fires_total == (uint64_t)fires,
          "fires_total=%llu != observed %d",
          (unsigned long long)a.fires_total, fires);
}

/* ── Test 8: metrics_from_batch derives AAS + lock fraction correctly ──── */
static void test_metrics_from_batch(void)
{
    printf("--- metrics_from_batch: AAS + lock-fraction derivation ---\n");

    /* A batch as build_batch would produce (T2: gated on-CPU records are
     * first-class CPU samples with new_event == 0). Mix of CPU, Lock,
     * LWLock, IO and idle (ClientRead / Activity), plus an io_worker
     * record that must be excluded. */
    struct pgwt_trace_event batch[8];
    memset(batch, 0, sizeof(batch));
    batch[0].new_event = WEI(PG_WAIT_LOCK, 0x03);     /* active, lock */
    batch[1].new_event = WEI(PG_WAIT_LOCK, 0x01);     /* active, lock */
    batch[2].new_event = WEI(PG_WAIT_LWLOCK, 0x07);   /* active, not lock */
    batch[3].new_event = WEI(PG_WAIT_IO, 0x10);       /* active, not lock */
    batch[4].new_event = WEI(PG_WAIT_CLIENT, 0);      /* idle (ClientRead) */
    batch[5].new_event = WEI(PG_WAIT_ACTIVITY, 0x02); /* idle (Activity) */
    batch[6].new_event = 0;                           /* on-CPU: ACTIVE (T2) */
    batch[7].new_event = WEI(PG_WAIT_IO, 0x10);       /* io_worker: excluded */
    batch[7].flags     = PGWT_EVENT_FLAG_IO_WORKER;

    double aas = -1, frac = -1;
    pgwt_anomaly_metrics_from_batch(batch, 8, &aas, &frac);
    /* Active = 5 (two idle + the io_worker excluded, the CPU sample
     * included); locks = 2 → fraction 0.4. */
    CHECK(aas == 5.0, "aas=%.1f expected 5", aas);
    CHECK(frac == 0.4, "lock_fraction=%.2f expected 0.40", frac);

    /* All-idle batch → AAS 0, fraction 0 (no divide-by-zero). */
    struct pgwt_trace_event idle[2];
    memset(idle, 0, sizeof(idle));
    idle[0].new_event = WEI(PG_WAIT_CLIENT, 0);
    idle[1].new_event = WEI(PG_WAIT_ACTIVITY, 0x01);
    pgwt_anomaly_metrics_from_batch(idle, 2, &aas, &frac);
    CHECK(aas == 0.0, "all-idle aas=%.1f expected 0", aas);
    CHECK(frac == 0.0, "all-idle frac=%.2f expected 0", frac);
}

/* ── Test 8b: a pure CPU storm must be able to trip the AAS rule (AAS-1) ─ */
static void test_cpu_storm_fires(void)
{
    printf("--- CPU storm: we==0 samples drive the AAS rule ---\n");

    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_factor = 3.0;
    a.aas_ticks  = 3;

    uint64_t clk = TICK_NS;
    warm_baseline(&a, 1.0, &clk);   /* quiet baseline ~1 */

    /* A CPU-storm batch: 8 client backends all on-CPU inside a command —
     * exactly the incident class the pre-T2 sampler was blind to (it
     * reported AAS 0.017 for a machine-saturating -S run; study Q4). */
    struct pgwt_trace_event storm[8];
    memset(storm, 0, sizeof(storm));   /* new_event = 0 = CPU, no flags */

    int fired = 0;
    for (int t = 0; t < 5; t++) {
        double aas = -1, frac = -1;
        pgwt_anomaly_metrics_from_batch(storm, 8, &aas, &frac);
        CHECK(aas == 8.0, "storm tick aas=%.1f expected 8", aas);
        struct pgwt_anomaly_decision d = pgwt_anomaly_eval(&a, aas, frac, clk);
        clk += TICK_NS;
        if (d.action == PGWT_ANOMALY_FIRE) {
            CHECK(d.fired_mask & PGWT_RULE_AAS, "CPU storm fires the AAS rule");
            fired = 1;
            break;
        }
    }
    CHECK(fired, "sustained CPU storm must FIRE (the anomaly engine was "
          "blind to exactly this before T2)");

    /* The same storm made of io_worker records must NOT fire — io_workers
     * are excluded from AAS by decision. */
    struct pgwt_anomaly b;
    pgwt_anomaly_init(&b, true, 10);
    b.aas_factor = 3.0;
    b.aas_ticks  = 3;
    clk = TICK_NS;
    warm_baseline(&b, 1.0, &clk);
    struct pgwt_trace_event iostorm[8];
    memset(iostorm, 0, sizeof(iostorm));
    for (int i = 0; i < 8; i++) {
        iostorm[i].new_event = WEI(PG_WAIT_IO, 0x01);
        iostorm[i].flags = PGWT_EVENT_FLAG_IO_WORKER;
    }
    for (int t = 0; t < 5; t++) {
        double aas = -1, frac = -1;
        pgwt_anomaly_metrics_from_batch(iostorm, 8, &aas, &frac);
        CHECK(aas == 0.0, "io_worker-only batch aas=%.1f expected 0", aas);
        struct pgwt_anomaly_decision d = pgwt_anomaly_eval(&b, aas, frac, clk);
        clk += TICK_NS;
        CHECK(d.action != PGWT_ANOMALY_FIRE,
              "io_worker load must never fire the AAS rule");
    }
}

/* ── Test 10: ESC-4 — lock rule needs a min-activity floor ─────────────── */
/* A single backend's routine 300 ms row-lock wait is lock_fraction 1.0 of an
 * AAS of 1 — the old rule fired and burned a 60 s window on OLTP noise. The
 * min-activity floor (lock-class AAS >= lock_min_aas) must veto that while
 * still firing on a genuine lock convoy. */
static void test_lock_min_activity(void)
{
    printf("--- ESC-4: lock rule requires a minimum lock-class AAS ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_factor    = 0.0;         /* isolate the lock rule */
    a.lock_fraction = 0.30;
    a.lock_min_aas  = 2.0;         /* default floor */
    a.lock_ticks    = 3;
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 1.0, &clk);

    /* Lone backend fully blocked on a lock: fraction 1.0 but lock-AAS = 1.0
     * (1 * 1.0) < 2.0 → must NEVER fire, however long it lasts. */
    int fires = 0;
    feed(&a, 1.0, 1.0, 100, &clk, &fires);
    CHECK(fires == 0, "single-backend lock wait fired %d times (must be 0)",
          fires);
    CHECK(a.lock_over_streak == 0,
          "min-activity floor keeps lock_over_streak at 0 (got %d)",
          a.lock_over_streak);

    /* A real convoy: AAS 6, lock share 0.7 → lock-AAS 4.2 >= 2.0 AND share
     * 0.7 > 0.30 → fires after the sustain count. */
    struct pgwt_anomaly b;
    pgwt_anomaly_init(&b, true, 10);
    b.aas_factor    = 0.0;
    b.lock_fraction = 0.30;
    b.lock_min_aas  = 2.0;
    b.lock_ticks    = 3;
    clk = TICK_NS;
    warm_baseline(&b, 1.0, &clk);
    int convoy_fires = 0;
    feed(&b, 6.0, 0.7, 5, &clk, &convoy_fires);
    CHECK(convoy_fires >= 1, "a real lock convoy MUST fire (got %d)",
          convoy_fires);

    /* Boundary: exactly at the floor (lock-AAS == 2.0) fires; just under does
     * not. AAS 4, share 0.5 => lock-AAS 2.0 (>= floor). */
    struct pgwt_anomaly c;
    pgwt_anomaly_init(&c, true, 10);
    c.aas_factor = 0.0; c.lock_fraction = 0.30; c.lock_min_aas = 2.0;
    c.lock_ticks = 2;
    clk = TICK_NS;
    warm_baseline(&c, 1.0, &clk);
    int at_floor = 0;
    feed(&c, 4.0, 0.5, 4, &clk, &at_floor);
    CHECK(at_floor >= 1, "lock-AAS exactly at the floor fires (got %d)",
          at_floor);
}

/* ── Test 11: ESC-7 — short incident protected, sustained regime learned ─ */
static void test_baseline_learn_through(void)
{
    printf("--- ESC-7: short incident protected; sustained regime learned ---\n");

    /* Property 1 (incident protection preserved): a burst well under the
     * learn-through horizon must NOT move the baseline. */
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_factor = 3.0;
    a.aas_ticks  = 3;
    /* Shrink the horizon so the test is fast but still >> the incident. */
    a.learn_through_ticks = 2000;   /* 200 s at 10 Hz */
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 2.0, &clk);
    double base0 = a.baseline_aas;
    feed(&a, 20.0, 0.0, 300, &clk, NULL);   /* 30 s incident << 200 s horizon */
    CHECK(a.baseline_aas < base0 + 1.0,
          "short incident must not raise baseline (%.2f -> %.2f)",
          base0, a.baseline_aas);

    /* Property 2 (learn-through): a regime change sustained PAST the horizon
     * must eventually be adopted so the rule stops re-firing forever. */
    struct pgwt_anomaly b;
    pgwt_anomaly_init(&b, true, 10);
    b.aas_factor = 3.0;
    b.aas_ticks  = 3;
    b.learn_through_ticks = 2000;
    b.slow_release_div    = 10;
    clk = TICK_NS;
    warm_baseline(&b, 2.0, &clk);
    double base_start = b.baseline_aas;
    /* Hold AAS=20 for well past the horizon + enough slow-EWMA memory to climb.
     * Slow alpha = (1/600)/10; ~2000+ extra ticks after the horizon lets the
     * baseline move a meaningful fraction toward 20. */
    feed(&b, 20.0, 0.0, 2000, &clk, NULL);   /* reach the horizon (no move) */
    double base_at_horizon = b.baseline_aas;
    CHECK(base_at_horizon < base_start + 1.0,
          "baseline still protected right up to the horizon (%.2f)",
          base_at_horizon);
    feed(&b, 20.0, 0.0, 30000, &clk, NULL);  /* learn through past the horizon */
    CHECK(b.baseline_aas > base_at_horizon + 2.0,
          "sustained regime change IS learned through (%.2f -> %.2f, toward 20)",
          base_at_horizon, b.baseline_aas);
    CHECK(b.baseline_aas < 20.0,
          "learn-through is SLOW, not instant (baseline %.2f still < 20)",
          b.baseline_aas);
}

/* ── Test 9: AAS + lock can fire together (combined fired_mask) ────────── */
static void test_combined_fire(void)
{
    printf("--- combined: AAS and lock fire on the same tick ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_factor    = 3.0;
    a.aas_ticks     = 2;
    a.lock_fraction = 0.30;
    a.lock_ticks    = 2;
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 2.0, &clk);

    /* Spike AAS=10 (>6) AND lock_frac 0.8 simultaneously, sustained 2 ticks. */
    pgwt_anomaly_eval(&a, 10.0, 0.8, clk); clk += TICK_NS;
    struct pgwt_anomaly_decision d = pgwt_anomaly_eval(&a, 10.0, 0.8, clk);
    CHECK(d.action == PGWT_ANOMALY_FIRE, "combined expected FIRE, got %d",
          d.action);
    CHECK((d.fired_mask & PGWT_RULE_AAS) && (d.fired_mask & PGWT_RULE_LOCK),
          "combined fired_mask=%u expected both AAS+LOCK", d.fired_mask);
}

int main(void)
{
    test_disabled();
    test_aas_sustained();
    test_aas_no_fire();
    test_busy_baseline_secondary_fires();
    test_idle_baseline_primary_still_fires();
    test_busy_baseline_jitter_does_not_fire();
    test_large_baseline_small_jump_does_not_fire();
    test_large_baseline_real_spike_fires();
    test_aas_factor_disable_contract();
    test_lock_fraction();
    test_cooldown();
    test_baseline_protected();
    test_budget_boundary();
    test_metrics_from_batch();
    test_cpu_storm_fires();
    test_lock_min_activity();
    test_baseline_learn_through();
    test_combined_fire();

    printf("\n%d/%d checks passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
