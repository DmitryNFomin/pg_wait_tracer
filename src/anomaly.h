/* anomaly.h — Anomaly-triggered escalation rules (Track A, D5 / Phase A5)
 *
 * In --mode tiered the always-on sampled stream is watched by a small rules
 * engine. When the live samples show an incident, the daemon AUTO-escalates to
 * full-fidelity capture (via the A4 escalation engine) so the full data for
 * the incident is on disk without a human reacting in time.
 *
 * Three rules, all evaluated once per sampler tick on metrics derived from that
 * tick's sample batch (no re-reading the trace):
 *
 *   1. AAS-vs-baseline — maintain a rolling baseline of AAS (active sessions,
 *      idle-excluded per pgwt_is_idle_event). Fire when
 *      `aas > k * rolling_baseline` is sustained for N consecutive ticks.
 *      Config: --anomaly-aas-factor k, --anomaly-aas-ticks N.
 *   2. Lock-class fraction — fire when the Lock-class share of the tick's
 *      ACTIVE (non-idle) samples exceeds a threshold, sustained for N ticks.
 *      Config: --anomaly-lock-fraction.
 *   3. CPU-demand saturation — a baseline-independent, one-sided CUSUM over
 *      CPU-class AAS divided by the postmaster's effective CPU capacity. The
 *      CUSUM advances by nominal sample time (never a delayed wall-clock gap),
 *      drains below its slack point, and fires at its evidence threshold.
 *      Config: --anomaly-cpu-cusum-k/-h/-cap and
 *      --anomaly-cpu-cusum-disable.
 *
 * Hysteresis + cooldown: once an auto-escalation fires, a minimum cooldown
 * (--anomaly-cooldown-s) must elapse before another auto-escalation can fire,
 * so a flapping metric cannot burst-burn the budget. The existing AAS/lock
 * rules may EXTEND an active window (pgwt_escalate's extend path), never stack
 * one; the CPU rule does not accumulate while a window or cooldown is active.
 *
 * Budget: a firing baseline/lock rule uses PGWT_ESC_REASON_ANOMALY; a firing
 * CPU rule uses the distinct PGWT_ESC_REASON_CPU_SATURATION. Both call the
 * same pgwt_escalate path. The A4 engine enforces the rolling-hour budget; an
 * over-budget trigger is dropped SILENTLY (logged, no error to anyone) —
 * unlike a manual escalate, which returns a denial to its caller.
 *
 * Observability: every NEAR-trigger (a metric crossed its threshold but the
 * sustained count was not yet met, or the fire was blocked by cooldown or
 * budget) is logged so thresholds can be tuned from real data.
 *
 * The rule logic is split out as a PURE function (pgwt_anomaly_eval) that
 * takes a tick's (aas, lock_fraction) and the current time and returns a
 * decision, mutating only the rule state. It touches neither BPF nor the
 * escalation engine, so it is unit-testable by feeding scripted sample
 * streams (see tests/test_anomaly.c). The daemon-side wrapper
 * (pgwt_anomaly_tick) derives the metrics from the live batch, logs the
 * decision, and performs the escalate on a FIRE.
 */
#ifndef PGWT_ANOMALY_H
#define PGWT_ANOMALY_H

#include <stdbool.h>
#include <stdint.h>

struct pgwt_daemon;
struct pgwt_trace_event;

/* What the rule engine decided this tick. */
enum pgwt_anomaly_action {
    PGWT_ANOMALY_NONE = 0,   /* nothing crossed; quiet tick */
    PGWT_ANOMALY_NEAR,       /* a threshold crossed but did not fire (see why) */
    PGWT_ANOMALY_FIRE,       /* a rule fired — escalate to full fidelity */
};

/* Why a near-trigger did not fire (bitmask; informational, for logging). */
enum pgwt_anomaly_near {
    PGWT_NEAR_NONE        = 0,
    PGWT_NEAR_AAS_SUSTAIN = 1 << 0, /* AAS over threshold, sustain count not met */
    PGWT_NEAR_LOCK_SUSTAIN = 1 << 1,/* lock-fraction over, sustain count not met */
    PGWT_NEAR_COOLDOWN    = 1 << 2, /* would fire but in cooldown */
    PGWT_NEAR_BASELINE    = 1 << 3, /* baseline not warm yet (no AAS rule) */
};

/* Which rule(s) triggered the fire (bitmask). Recorded for logs/diagnostics. */
enum pgwt_anomaly_rule {
    PGWT_RULE_NONE = 0,
    PGWT_RULE_AAS  = 1 << 0,
    PGWT_RULE_LOCK = 1 << 1,
    PGWT_RULE_CPU_SATURATION = 1 << 2,
};

/* Complete input for one pure rule evaluation. CPU evidence is deliberately
 * explicit so tests can exercise coverage/capacity/window gates without a
 * daemon. cpu_capacity <= 0 means UNKNOWN. cpu_guard_blocked is true while a
 * full-fidelity escalation window is already active; the core additionally
 * derives cooldown blocking from last_fire_ns. */
struct pgwt_anomaly_observation {
    double aas;
    double lock_fraction;
    double cpu_aas;
    double cpu_capacity;
    bool   cpu_coverage_ok;
    bool   cpu_capacity_changed;
    bool   cpu_guard_blocked;
};

struct pgwt_anomaly_decision {
    enum pgwt_anomaly_action action;
    unsigned near_mask;     /* enum pgwt_anomaly_near (when action == NEAR) */
    unsigned fired_mask;    /* enum pgwt_anomaly_rule (when action == FIRE) */
    double   aas;           /* metrics evaluated this tick (for logging) */
    double   baseline;      /* current rolling baseline AAS */
    double   lock_fraction; /* lock-class share of active samples this tick */
    double   cpu_aas;       /* CPU-class AAS (we==0, excluding io_worker) */
    double   cpu_capacity;  /* effective cores, or UNKNOWN (<=0) */
    double   cpu_cusum;     /* CPU CUSUM after this tick */
};

struct pgwt_anomaly {
    bool   enabled;           /* armed only in --mode tiered */

    /* Config (thresholds). Conservative defaults; all overridable. */
    double aas_factor;        /* fire when aas > factor * baseline */
    int    aas_ticks;         /* consecutive ticks the AAS rule must hold */
    double lock_fraction;     /* fire when lock share > this */
    double lock_min_aas;      /* ESC-4: AND lock-class AAS >= this (min-activity floor) */
    int    lock_ticks;        /* consecutive ticks the lock rule must hold */
    uint64_t cooldown_ns;     /* min gap between auto-escalations */
    int    escalation_s;      /* duration requested per auto-escalation */

    /* CPU-demand saturation CUSUM. cpu_cusum_enabled is independently
     * configurable. The legacy operational disable convention
     * (aas_factor >= PGWT_ANOMALY_DISABLE_AAS_FACTOR) also disables it. */
    bool   cpu_cusum_enabled;
    double cpu_cusum_k;       /* utilization slack/reference point */
    double cpu_cusum_h;       /* evidence threshold in nominal seconds */
    double cpu_cusum_cap;     /* cap applied to cpu_aas / capacity */
    double sample_period_s;   /* NOMINAL 1/sample_rate_hz, never wall gap */
    double cpu_cusum;         /* O(1) accumulated saturation evidence */

    /* ESC-7 baseline slow learn-through: after the AAS metric has been
     * continuously over the multiplicative threshold for learn_through_ticks,
     * the baseline resumes EWMA at 1/slow_release_div the normal rate, so a
     * sustained legitimate regime change is eventually adopted (the rule stops
     * re-firing forever) while a short incident (streak < learn_through_ticks)
     * still cannot raise the bar. */
    int    learn_through_ticks; /* consecutive over-ticks before learn-through */
    int    slow_release_div;    /* EWMA rate divisor while learning through */

    /* Rolling-baseline (EWMA) state for the AAS rule. The baseline tracks the
     * NORMAL load; it is NOT updated while a metric is anomalous (so a long
     * incident does not poison the baseline and silence the rule). */
    double  baseline_aas;     /* exponentially-weighted moving average */
    double  baseline_alpha;   /* EWMA smoothing factor (per tick) */
    int     baseline_warmup;  /* ticks observed before baseline is trustworthy */
    int     warmup_needed;    /* ticks of normal data required to warm up */

    /* Hysteresis: consecutive-over-threshold counters. */
    int     aas_over_streak;
    int     lock_over_streak;

    /* Cooldown: monotonic ns of the last auto-escalation (0 = never). */
    uint64_t last_fire_ns;

    /* ESC-8 near-trigger log rate-limit (daemon wrapper only): log on a change
     * of the near reason mask + a periodic summary, not every tick. */
    unsigned last_near_mask;    /* near_mask at the last emitted near-log */
    uint64_t last_near_log_ns;  /* monotonic ns of the last near-log line */
    uint64_t near_since_log;    /* near-triggers suppressed since the last line */

    /* Lifetime stats (control-socket metrics). */
    uint64_t fires_total;       /* auto-escalations actually requested */
    uint64_t cpu_saturation_fires_total; /* CPU-rule FIRE decisions */
    uint64_t near_total;        /* near-triggers logged */
    uint64_t dropped_budget;    /* fires the budget silently dropped */
    uint64_t dropped_cooldown;  /* fires the cooldown suppressed */
};

/* Default thresholds (conservative). */
#define PGWT_ANOMALY_DEF_AAS_FACTOR   3.0
#define PGWT_ANOMALY_DEF_AAS_TICKS    3
#define PGWT_ANOMALY_DEF_LOCK_FRAC    0.30
#define PGWT_ANOMALY_DEF_LOCK_MIN_AAS 2.0   /* ESC-4: min lock-class AAS to fire */
#define PGWT_ANOMALY_DEF_LOCK_TICKS   3
#define PGWT_ANOMALY_DEF_COOLDOWN_S   120
#define PGWT_ANOMALY_DEF_ESCALATE_S   60
#define PGWT_ANOMALY_DEF_CPU_CUSUM_K   0.80
#define PGWT_ANOMALY_DEF_CPU_CUSUM_H   1.50
#define PGWT_ANOMALY_DEF_CPU_CUSUM_CAP 1.25
/* Existing smoke/operational contract: this sentinel disables automatic AAS
 * anomaly behavior. Stage 3 also disables the CPU guard at this value. */
#define PGWT_ANOMALY_DISABLE_AAS_FACTOR 1000000.0
/* ESC-7 baseline slow learn-through (see struct comment). */
#define PGWT_ANOMALY_DEF_LEARN_THROUGH_MIN 15  /* minutes continuously-over before learn-through */
#define PGWT_ANOMALY_DEF_SLOW_RELEASE_DIV  10  /* learn at 1/10 the normal EWMA rate */

/* Initialize rule state + config. enabled is true only for tiered mode.
 * sample_rate_hz tunes the EWMA so the baseline reacts over roughly a fixed
 * wall-clock horizon regardless of tick rate. */
void pgwt_anomaly_init(struct pgwt_anomaly *a, bool enabled,
                       int sample_rate_hz);

/* ── Pure rule evaluation (no BPF, no escalation, unit-testable) ────────── */

/* Evaluate the rules for one tick. `obs` carries total AAS, lock share, and
 * the CPU guard's coverage/capacity inputs. `now_ns` is used only for the
 * existing cooldown; CPU evidence always advances by sample_period_s.
 *
 * Mutates only *a (baseline EWMA, streak counters, cooldown clock on a FIRE,
 * lifetime stats). Returns the decision. It does NOT call pgwt_escalate — the
 * caller does that on PGWT_ANOMALY_FIRE so the rule logic stays pure.
 *
 * Cooldown is charged here on FIRE so the test can assert flap suppression
 * without a live escalation engine: a FIRE updates last_fire_ns; a would-fire
 * inside cooldown is downgraded to NEAR(COOLDOWN). The BUDGET drop is NOT
 * modeled here (the engine owns the budget) — the daemon wrapper observes a
 * silent budget denial and records dropped_budget. */
struct pgwt_anomaly_decision
pgwt_anomaly_eval_observation(struct pgwt_anomaly *a,
                              const struct pgwt_anomaly_observation *obs,
                              uint64_t now_ns);

/* Compatibility entry point for the original AAS/lock-only core. CPU
 * capacity is UNKNOWN, so this never advances the CPU guard. */
struct pgwt_anomaly_decision
pgwt_anomaly_eval(struct pgwt_anomaly *a, double aas, double lock_fraction,
                  uint64_t now_ns);

/* Derive (aas, lock_fraction, cpu_aas) from a tick's encoded sample batch.
 * `samples` is the build_batch output (idle/on-CPU handling already applied
 * by the sampler for on-CPU; idle waits like ClientRead are still present and
 * are excluded here). Pure — exposed for the unit test. */
void pgwt_anomaly_metrics_from_batch(const struct pgwt_trace_event *samples,
                                     int n, double *out_aas,
                                     double *out_lock_fraction,
                                     double *out_cpu_aas);

/* ── Daemon-side wrapper (links the rules to the escalation engine) ─────── */

/* Called by the sampler each tick AFTER building its batch. Derives the
 * metrics, evaluates the rules, logs near-triggers, and on a FIRE requests an
 * anomaly escalation (silently dropped if over budget). No-op when disabled. */
void pgwt_anomaly_tick(struct pgwt_daemon *d,
                       const struct pgwt_trace_event *samples, int n);

#endif /* PGWT_ANOMALY_H */
