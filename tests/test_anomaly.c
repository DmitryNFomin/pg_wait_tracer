/* test_anomaly.c — Unit tests for the anomaly rule engine (Phase A5)
 *
 * Drives the PURE rule core (pgwt_anomaly_eval / pgwt_anomaly_metrics_from_batch)
 * with scripted sample-stream inputs and asserts fire / no-fire across:
 *   - AAS-vs-baseline (factor, sustained N ticks, baseline warmup)
 *   - lock-class fraction (threshold, sustained N ticks)
 *   - hysteresis / cooldown (a flapping metric cannot re-fire inside cooldown)
 *   - baseline-independent CPU-demand CUSUM with realistic noisy integer
 *     incidents, one-hour false-positive streams, and evidence-quality gates
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

/* Evaluate one complete Stage-3 observation and advance the scripted clock by
 * exactly one nominal 10 Hz tick. Wall-clock jumps are made explicitly by the
 * few tests that prove delayed callbacks do not add extra CPU evidence. */
static struct pgwt_anomaly_decision
cpu_tick_source(struct pgwt_anomaly *a, double aas, double cpu_aas,
                double capacity, bool affinity_only, bool coverage_ok,
                bool capacity_changed, bool guard_blocked, uint64_t *clock)
{
    const struct pgwt_anomaly_observation obs = {
        .aas = aas,
        .lock_fraction = 0.0,
        .cpu_aas = cpu_aas,
        .cpu_capacity = capacity,
        .cpu_capacity_affinity_only = affinity_only,
        .cpu_coverage_ok = coverage_ok,
        .cpu_capacity_changed = capacity_changed,
        .cpu_guard_blocked = guard_blocked,
    };
    struct pgwt_anomaly_decision d =
        pgwt_anomaly_eval_observation(a, &obs, *clock);
    *clock += TICK_NS;
    return d;
}

static struct pgwt_anomaly_decision
cpu_tick(struct pgwt_anomaly *a, double aas, double cpu_aas, double capacity,
         bool coverage_ok, bool capacity_changed, bool guard_blocked,
         uint64_t *clock)
{
    return cpu_tick_source(a, aas, cpu_aas, capacity, false, coverage_ok,
                           capacity_changed, guard_blocked, clock);
}

static void isolate_cpu_rule(struct pgwt_anomaly *a)
{
    a->aas_factor = 0.0;
    a->lock_fraction = 0.0;
}

/* Exact discrete distribution from the realistic-noise contract. Multiplying
 * by 37 permutes [0,99], so every 100 ticks contain counts 2,23,50,23,2 of
 * CPU AAS 2,3,4,5,6 respectively (mean 4), without constant plateaus. */
static int jittery_storm_cpu(int tick)
{
    int q = (tick * 37 + 11) % 100;
    if (q < 2)  return 2;
    if (q < 25) return 3;
    if (q < 75) return 4;
    if (q < 98) return 5;
    return 6;
}

/* Drifted total-AAS baseline P(1,2,3)=.10,.60,.30 (mean 2.2). */
static int drifted_baseline_aas(int tick)
{
    int q = (tick * 7 + 3) % 10;
    if (q < 1) return 1;
    if (q < 7) return 2;
    return 3;
}

/* Noisy integer CPU baseline with mean 0.8 AAS. */
static int baseline_cpu_demand(int tick)
{
    return ((tick * 3 + 1) % 5) == 0 ? 0 : 1;
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
    printf("--- AAS rule: under factor never fires ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    a.aas_factor = 3.0;
    a.aas_ticks  = 3;
    uint64_t clk = TICK_NS;
    warm_baseline(&a, 4.0, &clk);   /* baseline ~4 */

    /* AAS = 8 < 3*4 = 12 → never fires even sustained for many ticks. */
    int fires = 0;
    feed(&a, 8.0, 0.0, 50, &clk, &fires);
    CHECK(fires == 0, "AAS under factor fired %d times", fires);
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

    double aas = -1, frac = -1, cpu = -1;
    pgwt_anomaly_metrics_from_batch(batch, 8, &aas, &frac, &cpu);
    /* Active = 5 (two idle + the io_worker excluded, the CPU sample
     * included); locks = 2 → fraction 0.4. */
    CHECK(aas == 5.0, "aas=%.1f expected 5", aas);
    CHECK(frac == 0.4, "lock_fraction=%.2f expected 0.40", frac);
    CHECK(cpu == 1.0, "cpu_aas=%.1f expected 1", cpu);

    /* All-idle batch → AAS 0, fraction 0 (no divide-by-zero). */
    struct pgwt_trace_event idle[2];
    memset(idle, 0, sizeof(idle));
    idle[0].new_event = WEI(PG_WAIT_CLIENT, 0);
    idle[1].new_event = WEI(PG_WAIT_ACTIVITY, 0x01);
    pgwt_anomaly_metrics_from_batch(idle, 2, &aas, &frac, &cpu);
    CHECK(aas == 0.0, "all-idle aas=%.1f expected 0", aas);
    CHECK(frac == 0.0, "all-idle frac=%.2f expected 0", frac);
    CHECK(cpu == 0.0, "all-idle cpu_aas=%.1f expected 0", cpu);
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
        double aas = -1, frac = -1, cpu = -1;
        pgwt_anomaly_metrics_from_batch(storm, 8, &aas, &frac, &cpu);
        CHECK(aas == 8.0, "storm tick aas=%.1f expected 8", aas);
        CHECK(cpu == 8.0, "storm tick cpu_aas=%.1f expected 8", cpu);
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
        double aas = -1, frac = -1, cpu = -1;
        pgwt_anomaly_metrics_from_batch(iostorm, 8, &aas, &frac, &cpu);
        CHECK(aas == 0.0, "io_worker-only batch aas=%.1f expected 0", aas);
        CHECK(cpu == 0.0, "io_worker-only cpu_aas=%.1f expected 0", cpu);
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

/* ── AAS-1 Stage 3: realistic noisy CPU-saturation incidents ─────────── */
static void test_cpu_cusum_incidents(void)
{
    printf("--- CPU CUSUM: noisy saturation incidents fire on capacity ---\n");

    /* Five-minute drifted prelude, then the exact jittery storm on C=4. The
     * baseline converges to 2.2, so storm max AAS 6 never exceeds 3x baseline;
     * the distinct CPU rule must be the sole cause of escalation. */
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    uint64_t clk = TICK_NS;
    for (int t = 0; t < 5 * 60 * 10; t++)
        cpu_tick(&a, drifted_baseline_aas(t), baseline_cpu_demand(t),
                 4.0, true, false, false, &clk);

    bool primary_crossed = false;
    int fire_tick = 0;
    unsigned fire_mask = 0;
    for (int t = 0; t < 90; t++) {
        int cpu = jittery_storm_cpu(t);
        double threshold = a.aas_factor * a.baseline_aas;
        if (cpu >= 2 && (double)cpu > threshold)
            primary_crossed = true;
        struct pgwt_anomaly_decision d =
            cpu_tick_source(&a, cpu, cpu, 4.0, true,
                            true, false, false, &clk);
        if (d.action == PGWT_ANOMALY_FIRE) {
            fire_tick = t + 1;
            fire_mask = d.fired_mask;
            break;
        }
    }
    CHECK(!primary_crossed,
          "3x total-AAS predicate crossed during C=4 storm (baseline %.2f)",
          a.baseline_aas);
    CHECK(fire_tick > 0 && fire_tick <= 90,
          "C=4 noisy storm fire tick=%d expected within 9s", fire_tick);
    CHECK((fire_mask & PGWT_RULE_CPU_SATURATION)
          && !(fire_mask & PGWT_RULE_AAS),
          "C=4 fire mask=%u expected CPU saturation only", fire_mask);
    CHECK(a.cpu_saturation_fires_total == 1,
          "C=4 cpu_saturation_fires_total=%llu expected 1",
          (unsigned long long)a.cpu_saturation_fires_total);
    int c4_fire_tick = fire_tick;

    /* Same prelude and stream on C=2: ratio capping makes the expected
     * evidence rate ~0.445/s, so it should fire in roughly 3.4 seconds. */
    struct pgwt_anomaly b;
    pgwt_anomaly_init(&b, true, 10);
    clk = TICK_NS;
    for (int t = 0; t < 5 * 60 * 10; t++)
        cpu_tick(&b, drifted_baseline_aas(t), baseline_cpu_demand(t),
                 2.0, true, false, false, &clk);
    fire_tick = 0;
    for (int t = 0; t < 40; t++) {
        int cpu = jittery_storm_cpu(t);
        struct pgwt_anomaly_decision d =
            cpu_tick_source(&b, cpu, cpu, 2.0, true,
                            true, false, false, &clk);
        if (d.action == PGWT_ANOMALY_FIRE) {
            fire_tick = t + 1;
            CHECK(d.fired_mask & PGWT_RULE_CPU_SATURATION,
                  "C=2 storm fired non-CPU mask=%u", d.fired_mask);
            break;
        }
    }
    CHECK(fire_tick >= 30 && fire_tick <= 40,
          "C=2 noisy storm fire tick=%d expected about 4s", fire_tick);
    int c2_fire_tick = fire_tick;

    /* No baseline warmup: the baseline-independent guard still fires. */
    struct pgwt_anomaly c;
    pgwt_anomaly_init(&c, true, 10);
    clk = TICK_NS;
    fire_tick = 0;
    for (int t = 0; t < 90; t++) {
        int cpu = jittery_storm_cpu(t);
        struct pgwt_anomaly_decision d =
            cpu_tick_source(&c, cpu, cpu, 4.0, true,
                            true, false, false, &clk);
        if (d.action == PGWT_ANOMALY_FIRE) {
            fire_tick = t + 1;
            CHECK(d.fired_mask & PGWT_RULE_CPU_SATURATION,
                  "cold-start storm fired non-CPU mask=%u", d.fired_mask);
            break;
        }
    }
    CHECK(fire_tick > 0 && fire_tick <= 90,
          "cold-start C=4 noisy storm fire tick=%d expected within 9s",
          fire_tick);

    /* The identical four-session-mean stream is nowhere near C=16. */
    struct pgwt_anomaly wide;
    pgwt_anomaly_init(&wide, true, 10);
    isolate_cpu_rule(&wide);
    clk = TICK_NS;
    int grants = 0;
    for (int t = 0; t < 60 * 10; t++) {
        int cpu = jittery_storm_cpu(t);
        struct pgwt_anomaly_decision d =
            cpu_tick_source(&wide, cpu, cpu, 16.0, true,
                            true, false, false, &clk);
        grants += d.action == PGWT_ANOMALY_FIRE;
    }
    CHECK(grants == 0 && wide.fires_total == 0,
          "C=16 storm consumed %d grants / %llu fires",
          grants, (unsigned long long)wide.fires_total);
    CHECK(wide.dropped_budget == 0,
          "C=16 storm recorded %llu budget drops",
          (unsigned long long)wide.dropped_budget);

    /* One arbitrarily large sample contributes only cap-k = .45 for .1s. */
    struct pgwt_anomaly capped;
    pgwt_anomaly_init(&capped, true, 10);
    isolate_cpu_rule(&capped);
    clk = TICK_NS;
    struct pgwt_anomaly_decision one =
        cpu_tick(&capped, 100.0, 100.0, 4.0, true, false, false, &clk);
    CHECK(one.action != PGWT_ANOMALY_FIRE,
          "one cpu_aas=100 tick bypassed the utilization cap");
    CHECK(capped.cpu_cusum > 0.044 && capped.cpu_cusum < 0.046,
          "one capped tick produced S=%.6f expected 0.045", capped.cpu_cusum);

    printf("  storm matrix: C=4 %.1fs, C=2 %.1fs, C=16 grants=%d\n",
           c4_fire_tick / 10.0, c2_fire_tick / 10.0, grants);
}

/* ── AAS-1 Stage 3: evidence-quality and no-banking gates ────────────── */
static void test_cpu_cusum_gates(void)
{
    printf("--- CPU CUSUM: coverage/capacity/change/cooldown gates ---\n");

    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    isolate_cpu_rule(&a);
    uint64_t clk = TICK_NS;
    for (int t = 0; t < 10; t++)
        cpu_tick(&a, 5.0, 5.0, 4.0, true, false, false, &clk);
    double before = a.cpu_cusum;
    struct pgwt_anomaly_decision low_coverage =
        cpu_tick(&a, 100.0, 100.0, 4.0, false, false, false, &clk);
    CHECK(low_coverage.action != PGWT_ANOMALY_FIRE,
          "low-coverage tick fired the CPU guard");
    CHECK(a.cpu_cusum == before,
          "low-coverage tick changed S %.6f -> %.6f", before, a.cpu_cusum);

    /* UNKNOWN is a stable disabled state, even under impossible demand. */
    struct pgwt_anomaly unknown;
    pgwt_anomaly_init(&unknown, true, 10);
    isolate_cpu_rule(&unknown);
    clk = TICK_NS;
    int grants = 0;
    for (int t = 0; t < 60 * 60 * 10; t++) {
        struct pgwt_anomaly_decision d =
            cpu_tick(&unknown, 100.0, 100.0, -1.0,
                     true, false, false, &clk);
        grants += d.action == PGWT_ANOMALY_FIRE;
    }
    CHECK(grants == 0 && unknown.cpu_cusum == 0.0,
          "UNKNOWN capacity produced %d grants, S=%.6f",
          grants, unknown.cpu_cusum);
    CHECK(unknown.fires_total == 0 && unknown.dropped_budget == 0,
          "UNKNOWN capacity consumed fire/budget accounting");

    /* A material C change invalidates evidence in the old units and skips the
     * change tick; stable observations may start accumulating only afterward. */
    struct pgwt_anomaly changed;
    pgwt_anomaly_init(&changed, true, 10);
    isolate_cpu_rule(&changed);
    clk = TICK_NS;
    for (int t = 0; t < 10; t++)
        cpu_tick(&changed, 5.0, 5.0, 4.0, true, false, false, &clk);
    CHECK(changed.cpu_cusum > 0.4, "change setup S=%.3f expected >0.4",
          changed.cpu_cusum);
    cpu_tick(&changed, 5.0, 5.0, 2.0, true, true, false, &clk);
    CHECK(changed.cpu_cusum == 0.0,
          "material capacity change did not reset S (%.6f)",
          changed.cpu_cusum);
    cpu_tick(&changed, 5.0, 5.0, 2.0, true, false, false, &clk);
    CHECK(changed.cpu_cusum > 0.044 && changed.cpu_cusum < 0.046,
          "post-change stable tick did not restart at one nominal step: %.6f",
          changed.cpu_cusum);

    /* Active windows clear evidence rather than banking an almost-fire. */
    cpu_tick(&changed, 5.0, 5.0, 2.0, true, false, true, &clk);
    CHECK(changed.cpu_cusum == 0.0,
          "active escalation window banked CPU evidence %.6f",
          changed.cpu_cusum);

    /* A delayed callback still adds one nominal .1s step, not the gap. */
    struct pgwt_anomaly delayed;
    pgwt_anomaly_init(&delayed, true, 10);
    isolate_cpu_rule(&delayed);
    clk = TICK_NS;
    cpu_tick(&delayed, 5.0, 5.0, 4.0, true, false, false, &clk);
    clk += 30ULL * 1000000000ULL;
    cpu_tick(&delayed, 5.0, 5.0, 4.0, true, false, false, &clk);
    CHECK(delayed.cpu_cusum > 0.089 && delayed.cpu_cusum < 0.091,
          "30s delayed tick produced S=%.6f expected two nominal steps",
          delayed.cpu_cusum);

    /* A fire resets S, and the subsequent cooldown cannot accumulate it. */
    struct pgwt_anomaly cooldown;
    pgwt_anomaly_init(&cooldown, true, 10);
    isolate_cpu_rule(&cooldown);
    clk = TICK_NS;
    bool fired = false;
    for (int t = 0; t < 100; t++) {
        struct pgwt_anomaly_decision d =
            cpu_tick(&cooldown, 5.0, 5.0, 4.0,
                     true, false, false, &clk);
        if (d.action == PGWT_ANOMALY_FIRE) {
            fired = true;
            break;
        }
    }
    CHECK(fired && cooldown.cpu_cusum == 0.0,
          "CPU fire did not reset S (fired=%d S=%.6f)",
          fired, cooldown.cpu_cusum);
    for (int t = 0; t < 100; t++)
        cpu_tick(&cooldown, 100.0, 100.0, 4.0,
                 true, false, false, &clk);
    CHECK(cooldown.cpu_cusum == 0.0
          && cooldown.cpu_saturation_fires_total == 1,
          "cooldown banked/refired CPU evidence (S=%.6f fires=%llu)",
          cooldown.cpu_cusum,
          (unsigned long long)cooldown.cpu_saturation_fires_total);
}

/* ── AAS-1 Stage 3 fix: one grant per saturation episode ─────────────── */
static void test_cpu_cusum_episode_latch(void)
{
    printf("--- CPU CUSUM: one fire per saturation episode ---\n");

    struct pgwt_anomaly chronic;
    pgwt_anomaly_init(&chronic, true, 10);
    isolate_cpu_rule(&chronic);
    uint64_t clk = TICK_NS;
    int grants = 0;
    for (int t = 0; t < 60 * 60 * 10; t++) {
        struct pgwt_anomaly_decision d =
            cpu_tick(&chronic, 4.0, 4.0, 4.0,
                     true, false, false, &clk);
        grants += d.action == PGWT_ANOMALY_FIRE;
    }
    CHECK(grants <= 1 && chronic.fires_total <= 1,
          "chronic saturation consumed %d grants / %llu fires in one hour",
          grants, (unsigned long long)chronic.fires_total);
    CHECK(grants == 1 && !chronic.cpu_armed,
          "chronic saturation expected one fire and disarmed state "
          "(grants=%d armed=%d)", grants, chronic.cpu_armed);
    CHECK(chronic.dropped_budget == 0,
          "chronic saturation touched budget-drop accounting");

    /* Neither a held partial-read observation nor UNKNOWN capacity proves
     * recovery. A complete known u<k tick does, after which a new sustained
     * saturation episode may fire once more. */
    cpu_tick(&chronic, 0.0, 0.0, 4.0,
             false, false, false, &clk);
    CHECK(!chronic.cpu_armed, "low-coverage tick re-armed CPU episode");
    cpu_tick(&chronic, 0.0, 0.0, -1.0,
             true, false, false, &clk);
    CHECK(!chronic.cpu_armed, "UNKNOWN-capacity tick re-armed CPU episode");
    cpu_tick(&chronic, 3.0, 3.0, 4.0,
             true, false, false, &clk);
    CHECK(chronic.cpu_armed && chronic.cpu_cusum == 0.0,
          "trusted below-k recovery did not re-arm drained CPU episode");

    int second_episode_ticks = 0;
    for (int t = 0; t < 100; t++) {
        struct pgwt_anomaly_decision d =
            cpu_tick(&chronic, 4.0, 4.0, 4.0,
                     true, false, false, &clk);
        if (d.action == PGWT_ANOMALY_FIRE) {
            second_episode_ticks = t + 1;
            break;
        }
    }
    CHECK(second_episode_ticks > 0 && chronic.fires_total == 2,
          "recover/resaturate did not produce exactly two episode fires "
          "(tick=%d fires=%llu)", second_episode_ticks,
          (unsigned long long)chronic.fires_total);

    printf("  chronic busy: grants/hour=%d; recover/resaturate fires=%llu\n",
           grants, (unsigned long long)chronic.fires_total);
}

/* ── AAS-1 Stage 3 fix: activity floor + affinity-only margin ────────── */
static void test_cpu_cusum_floor_and_margin(void)
{
    printf("--- CPU CUSUM: absolute floor and affinity capacity margin ---\n");

    /* Even a severe capacity underestimate cannot turn one CPU-active
     * backend into an escalation: the absolute activity floor is mandatory. */
    struct pgwt_anomaly floor;
    pgwt_anomaly_init(&floor, true, 10);
    isolate_cpu_rule(&floor);
    CHECK(floor.cpu_min_aas == 2.0 && floor.cpu_margin == 0.02,
          "CPU floor/margin defaults are %.3f/%.3f, expected 2.0/.02",
          floor.cpu_min_aas, floor.cpu_margin);
    uint64_t floor_clk = TICK_NS;
    int floor_grants = 0;
    for (int t = 0; t < 60 * 60 * 10; t++) {
        struct pgwt_anomaly_decision d =
            cpu_tick(&floor, 1.0, 1.0, 0.5,
                     true, false, false, &floor_clk);
        floor_grants += d.action == PGWT_ANOMALY_FIRE;
    }
    CHECK(floor_grants == 0 && floor.fires_total == 0,
          "cpu_aas below floor fired on underestimated C "
          "(%d grants / %llu fires)", floor_grants,
          (unsigned long long)floor.fires_total);

    /* Noisy integer demand has mean 3.24 on C=4 (u=.81): it eventually fires
     * with the base k=.80, but affinity-only provenance adds the default .02
     * margin and makes the same benign stream drain instead. */
    struct pgwt_anomaly affinity;
    struct pgwt_anomaly no_margin;
    pgwt_anomaly_init(&affinity, true, 10);
    pgwt_anomaly_init(&no_margin, true, 10);
    isolate_cpu_rule(&affinity);
    isolate_cpu_rule(&no_margin);
    uint64_t affinity_clk = TICK_NS;
    uint64_t no_margin_clk = TICK_NS;
    int affinity_grants = 0;
    int no_margin_grants = 0;
    for (int t = 0; t < 60 * 60 * 10; t++) {
        int q = (t * 37 + 11) % 100;
        int cpu = q < 76 ? 3 : 4;
        struct pgwt_anomaly_decision with =
            cpu_tick_source(&affinity, cpu, cpu, 4.0, true,
                            true, false, false, &affinity_clk);
        struct pgwt_anomaly_decision without =
            cpu_tick_source(&no_margin, cpu, cpu, 4.0, false,
                            true, false, false, &no_margin_clk);
        affinity_grants += with.action == PGWT_ANOMALY_FIRE;
        no_margin_grants += without.action == PGWT_ANOMALY_FIRE;
    }
    CHECK(affinity_grants == 0 && affinity.fires_total == 0,
          "affinity margin did not suppress benign u=.81 load "
          "(%d grants / %llu fires)", affinity_grants,
          (unsigned long long)affinity.fires_total);
    CHECK(no_margin_grants > 0,
          "margin pinning stream without affinity margin never fired");

    printf("  underestimated floor grants=%d; affinity u=.81 grants=%d "
           "(zero-margin control=%d)\n",
           floor_grants, affinity_grants, no_margin_grants);
}

/* ── AAS-1 Stage 3 fix: discard evidence across partial-read gaps ────── */
static void test_cpu_cusum_coverage_return(void)
{
    printf("--- CPU CUSUM: coverage return resets stale evidence ---\n");

    struct pgwt_anomaly idle_return;
    pgwt_anomaly_init(&idle_return, true, 10);
    isolate_cpu_rule(&idle_return);
    uint64_t clk = TICK_NS;
    for (int t = 0; t < 33; t++)
        cpu_tick(&idle_return, 5.0, 5.0, 4.0,
                 true, false, false, &clk);
    CHECK(idle_return.cpu_cusum > 1.48 && idle_return.cpu_cusum < 1.49,
          "coverage-gap setup S=%.6f expected just below h",
          idle_return.cpu_cusum);
    for (int t = 0; t < 100; t++)
        cpu_tick(&idle_return, 0.0, 0.0, 4.0,
                 false, false, false, &clk);
    struct pgwt_anomaly_decision idle =
        cpu_tick(&idle_return, 0.0, 0.0, 4.0,
                 true, false, false, &clk);
    CHECK(idle.action != PGWT_ANOMALY_FIRE && idle_return.cpu_cusum == 0.0,
          "idle coverage return fired stale evidence (action=%d S=%.6f)",
          idle.action, idle_return.cpu_cusum);

    struct pgwt_anomaly busy_return;
    pgwt_anomaly_init(&busy_return, true, 10);
    isolate_cpu_rule(&busy_return);
    clk = TICK_NS;
    for (int t = 0; t < 33; t++)
        cpu_tick(&busy_return, 5.0, 5.0, 4.0,
                 true, false, false, &clk);
    for (int t = 0; t < 100; t++)
        cpu_tick(&busy_return, 0.0, 0.0, 4.0,
                 false, false, false, &clk);
    struct pgwt_anomaly_decision busy =
        cpu_tick(&busy_return, 5.0, 5.0, 4.0,
                 true, false, false, &clk);
    CHECK(busy.action != PGWT_ANOMALY_FIRE
          && busy_return.cpu_cusum > 0.044
          && busy_return.cpu_cusum < 0.046,
          "busy coverage return retained stale S (action=%d S=%.6f)",
          busy.action, busy_return.cpu_cusum);
    int clean_ticks = 1;
    while (busy.action != PGWT_ANOMALY_FIRE && clean_ticks < 100) {
        busy = cpu_tick(&busy_return, 5.0, 5.0, 4.0,
                        true, false, false, &clk);
        clean_ticks++;
    }
    CHECK(busy.action == PGWT_ANOMALY_FIRE
          && (busy.fired_mask & PGWT_RULE_CPU_SATURATION),
          "continued saturation after coverage return did not fire "
          "(ticks=%d action=%d)", clean_ticks, busy.action);

    printf("  stale idle return grants=0; sustained return fire=%.1fs\n",
           clean_ticks / 10.0);
}

/* ── AAS-1 Stage 3 fix: surface a climbing half-threshold CUSUM ──────── */
static void test_cpu_cusum_near_flag(void)
{
    printf("--- CPU CUSUM: near flag while evidence climbs ---\n");
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    isolate_cpu_rule(&a);
    uint64_t clk = TICK_NS;
    struct pgwt_anomaly_decision d = {0};
    for (int t = 0; t < 17; t++)
        d = cpu_tick(&a, 5.0, 5.0, 4.0,
                     true, false, false, &clk);
    CHECK(d.action == PGWT_ANOMALY_NEAR
          && (d.near_mask & PGWT_NEAR_CPU_SUSTAIN),
          "S=%.6f expected NEAR(cpu-sustain), action=%d mask=%u",
          d.cpu_cusum, d.action, d.near_mask);
    CHECK(!(d.fired_mask & PGWT_RULE_CPU_SATURATION),
          "CPU near tick also reported a fire mask=%u", d.fired_mask);
}

/* ── AAS-1 Stage 3: explicit and legacy disable contracts ────────────── */
static void test_cpu_cusum_disable_contract(void)
{
    printf("--- CPU CUSUM: independent + legacy disable contracts ---\n");
    uint64_t clk = TICK_NS;

    struct pgwt_anomaly independent;
    pgwt_anomaly_init(&independent, true, 10);
    isolate_cpu_rule(&independent);
    independent.cpu_cusum_enabled = false;
    int grants = 0;
    for (int t = 0; t < 1000; t++) {
        struct pgwt_anomaly_decision d =
            cpu_tick(&independent, 100.0, 100.0, 4.0,
                     true, false, false, &clk);
        grants += d.action == PGWT_ANOMALY_FIRE;
    }
    CHECK(grants == 0 && independent.cpu_cusum == 0.0,
          "independent disable produced %d grants / S %.3f",
          grants, independent.cpu_cusum);

    struct pgwt_anomaly legacy;
    pgwt_anomaly_init(&legacy, true, 10);
    legacy.aas_factor = PGWT_ANOMALY_DISABLE_AAS_FACTOR;
    legacy.lock_fraction = 0.0;
    clk = TICK_NS;
    grants = 0;
    for (int t = 0; t < 1000; t++) {
        struct pgwt_anomaly_decision d =
            cpu_tick(&legacy, 100.0, 100.0, 4.0,
                     true, false, false, &clk);
        grants += d.action == PGWT_ANOMALY_FIRE;
    }
    CHECK(grants == 0 && legacy.fires_total == 0 && legacy.cpu_cusum == 0.0,
          "aas-factor disable produced %d grants / %llu fires / S %.3f",
          grants, (unsigned long long)legacy.fires_total, legacy.cpu_cusum);
}

/* Drive one of the required hour-long false-positive workloads. All values
 * presented to the detector are integers; fractional labels below are the
 * means of their deterministic jitter patterns. */
static void run_quiet_hour(int scenario, const char *name)
{
    struct pgwt_anomaly a;
    pgwt_anomaly_init(&a, true, 10);
    isolate_cpu_rule(&a);
    uint64_t clk = TICK_NS;
    int grants = 0;

    for (int t = 0; t < 60 * 60 * 10; t++) {
        int aas = 0;
        int cpu = 0;
        switch (scenario) {
        case 0: { /* total AAS 1.5 <-> 3.5; CPU demand 0.4 <-> 1 */
            bool high = ((t / 10) & 1) != 0;
            aas = high ? 3 + (t & 1) : 1 + (t & 1);
            int q = t % 5;
            cpu = high ? (q == 0 ? 0 : (q == 4 ? 2 : 1))
                       : (q < 2 ? 1 : 0);
            break;
        }
        case 1: { /* adversarial CPU demand 1.5 <-> 3.5 */
            bool high = ((t / 10) & 1) != 0;
            cpu = high ? 3 + (t & 1) : 1 + (t & 1);
            aas = cpu;
            break;
        }
        case 2: /* OLTP total AAS 10..15, CPU demand about 1.5..2 */
            aas = 10 + ((t * 5 + t / 7) % 6);
            cpu = ((t / 10) & 1) ? 2 : 1 + (t & 1);
            break;
        case 3: { /* a 3s ~4-CPU burst once per minute */
            int within_minute = t % (60 * 10);
            if (within_minute < 3 * 10) {
                cpu = jittery_storm_cpu(within_minute);
            } else {
                cpu = baseline_cpu_demand(t);
            }
            aas = cpu;
            break;
        }
        case 4: /* post-lull: noisy mean 0.5 -> noisy mean 2 */
            if (t < 30 * 60 * 10)
                cpu = t & 1;
            else
                cpu = 1 + ((t * 7 + 1) % 3);
            aas = cpu;
            break;
        }
        struct pgwt_anomaly_decision d =
            cpu_tick(&a, aas, cpu, 4.0, true, false, false, &clk);
        grants += d.action == PGWT_ANOMALY_FIRE;
    }

    CHECK(grants == 0 && a.fires_total == 0,
          "%s: %d grants / %llu fires in one hour", name, grants,
          (unsigned long long)a.fires_total);
    CHECK(a.cpu_saturation_fires_total == 0,
          "%s: %llu CPU-rule fires in one hour", name,
          (unsigned long long)a.cpu_saturation_fires_total);
    CHECK(a.dropped_budget == 0,
          "%s: %llu budget drops in one hour", name,
          (unsigned long long)a.dropped_budget);
    printf("  normal[%s]: grants=%d fires=%llu drops=%llu\n",
           name, grants, (unsigned long long)a.fires_total,
           (unsigned long long)a.dropped_budget);
}

/* ── AAS-1 Stage 3: long false-positive matrix + boundaries ──────────── */
static void test_cpu_cusum_false_positive_matrix(void)
{
    printf("--- CPU CUSUM: one-hour realistic negative matrix ---\n");
    run_quiet_hour(0, "AAS 1.5<->3.5 / CPU .4<->1");
    run_quiet_hour(1, "adversarial CPU 1.5<->3.5");
    run_quiet_hour(2, "OLTP AAS 10..15 / CPU 1.5<->2");
    run_quiet_hour(3, "3s four-CPU burst once/min");
    run_quiet_hour(4, "post-lull .5->2");

    /* Boundary: noisy integer demand with mean exactly .8C has zero net
     * drift. A 37-stride permutation spreads the exact 80/20 mix of 3/4 CPU
     * ticks irregularly while bounding excursions over every 100-tick block. */
    struct pgwt_anomaly boundary;
    pgwt_anomaly_init(&boundary, true, 10);
    isolate_cpu_rule(&boundary);
    uint64_t clk = TICK_NS;
    double max_s = 0.0;
    int grants = 0;
    for (int t = 0; t < 60 * 60 * 10; t++) {
        int q = (t * 37 + 11) % 100;
        int cpu = q < 80 ? 3 : 4; /* mean 3.2 == .8C */
        struct pgwt_anomaly_decision d =
            cpu_tick(&boundary, cpu, cpu, 4.0,
                     true, false, false, &clk);
        if (boundary.cpu_cusum > max_s)
            max_s = boundary.cpu_cusum;
        grants += d.action == PGWT_ANOMALY_FIRE;
    }
    CHECK(grants == 0 && max_s < 0.061,
          "noisy mean .8C grants=%d max_S=%.6f", grants, max_s);

    /* The required once-per-minute burst shape contributes about .6, then
     * ordinary low demand drains it back to zero rather than banking it. */
    struct pgwt_anomaly burst;
    pgwt_anomaly_init(&burst, true, 10);
    isolate_cpu_rule(&burst);
    clk = TICK_NS;
    for (int t = 0; t < 3 * 10; t++) {
        int cpu = jittery_storm_cpu(t);
        cpu_tick(&burst, cpu, cpu, 4.0, true, false, false, &clk);
    }
    CHECK(burst.cpu_cusum > 0.62 && burst.cpu_cusum < 0.64,
          "3s noisy four-CPU burst contributes S=%.6f expected ~0.63",
          burst.cpu_cusum);
    for (int t = 0; t < 100; t++) {
        int cpu = baseline_cpu_demand(t);
        cpu_tick(&burst, cpu, cpu, 4.0, true, false, false, &clk);
    }
    CHECK(burst.cpu_cusum == 0.0,
          "post-burst low demand did not drain S (%.6f)", burst.cpu_cusum);

    /* Noisy mean .9C: mean drift is .1 evidence-seconds/s, so h=1.5 fires
     * at 15 seconds. */
    struct pgwt_anomaly high;
    pgwt_anomaly_init(&high, true, 10);
    isolate_cpu_rule(&high);
    clk = TICK_NS;
    int fire_tick = 0;
    for (int t = 0; t < 200; t++) {
        int q = (t * 7 + 3) % 10;
        int cpu = q < 4 ? 3 : 4; /* mean 3.6 == .9C */
        struct pgwt_anomaly_decision d =
            cpu_tick(&high, cpu, cpu, 4.0,
                     true, false, false, &clk);
        if (d.action == PGWT_ANOMALY_FIRE) {
            fire_tick = t + 1;
            break;
        }
    }
    CHECK(fire_tick >= 145 && fire_tick <= 155,
          "noisy mean .9C fire tick=%d expected about 150", fire_tick);

    /* Below-k ticks drain accumulated evidence; they do not hard-reset it. */
    struct pgwt_anomaly drain;
    pgwt_anomaly_init(&drain, true, 10);
    isolate_cpu_rule(&drain);
    clk = TICK_NS;
    for (int t = 0; t < 10; t++)
        cpu_tick(&drain, 5.0, 5.0, 4.0, true, false, false, &clk);
    double high_s = drain.cpu_cusum;
    cpu_tick(&drain, 0.0, 0.0, 4.0, true, false, false, &clk);
    CHECK(drain.cpu_cusum > 0.0 && drain.cpu_cusum < high_s,
          "one low tick should drain, not reset: %.3f -> %.3f",
          high_s, drain.cpu_cusum);
    CHECK(drain.cpu_cusum > 0.369 && drain.cpu_cusum < 0.371,
          "drain amount %.6f expected 0.370", drain.cpu_cusum);
}

int main(void)
{
    test_disabled();
    test_aas_sustained();
    test_aas_no_fire();
    test_lock_fraction();
    test_cooldown();
    test_baseline_protected();
    test_budget_boundary();
    test_metrics_from_batch();
    test_cpu_storm_fires();
    test_lock_min_activity();
    test_baseline_learn_through();
    test_combined_fire();
    test_cpu_cusum_incidents();
    test_cpu_cusum_gates();
    test_cpu_cusum_episode_latch();
    test_cpu_cusum_floor_and_margin();
    test_cpu_cusum_coverage_return();
    test_cpu_cusum_near_flag();
    test_cpu_cusum_disable_contract();
    test_cpu_cusum_false_positive_matrix();

    printf("\n%d/%d checks passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
