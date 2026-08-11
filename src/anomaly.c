/* anomaly.c — Anomaly-triggered escalation rules (Track A, D5 / Phase A5)
 *
 * See anomaly.h for the design. The pure rule core
 * (pgwt_anomaly_eval_observation and pgwt_anomaly_metrics_from_batch) is free
 * of BPF and the escalation engine so the unit test (tests/test_anomaly.c) can
 * drive it with scripted sample streams. The daemon-side wrapper
 * (pgwt_anomaly_tick) connects the FIRE decision to A4's pgwt_escalate() and
 * logs every near-trigger.
 *
 * The pure core is compiled into both the daemon and (via -DPGWT_SERVER) the
 * unit test; the daemon wrapper is guarded out of the server build.
 */
#include "anomaly.h"
#include "pg_wait_tracer.h"   /* WE_CLASS, PG_WAIT_LOCK, marker macros */

#include <stdio.h>
#include <string.h>

/* ── Pure rule core (no BPF, no escalation) ───────────────────────────── */

/* Idle waits (Activity class, Client:ClientRead) ARE in the batch but must
 * be excluded from "active sessions" exactly as the AAS/DB-Time views do. We
 * inline the same predicate here to keep the pure core dependency-free
 * (pgwt_is_idle_event lives in wait_event.c, which the unit test does not
 * link); the definition must stay in sync with pgwt_is_idle_event(). */
static bool sample_is_idle(uint32_t wei)
{
    return WE_CLASS(wei) == PG_WAIT_ACTIVITY        /* Activity class */
        || wei == WEI(PG_WAIT_CLIENT, 0);           /* Client:ClientRead */
}

void pgwt_anomaly_metrics_from_batch(const struct pgwt_trace_event *samples,
                                     int n, double *out_aas,
                                     double *out_lock_fraction,
                                     double *out_cpu_aas)
{
    int active = 0;
    int locks = 0;
    int cpu = 0;
    for (int i = 0; i < n; i++) {
        uint32_t we = samples[i].new_event;
        /* T2 (AAS-1): the decomposed model. we==0 records in the batch are
         * first-class CPU samples (the sampler already applied the
         * command-open gate / per-type policy) — an on-CPU session IS an
         * active session, so a pure CPU storm must move this metric.
         * io_worker samples never count (their busy time shadow-copies the
         * requesting backends' AioIoCompletion waits and is surfaced as a
         * utilization metric instead — docs/AAS_SEMANTICS_DECISION.md). */
        if (samples[i].flags & PGWT_EVENT_FLAG_IO_WORKER)
            continue;
        if (we != 0 && sample_is_idle(we))
            continue;   /* instrumented idle: not an active session */
        active++;
        if (we == 0)
            cpu++;
        if (WE_CLASS(we) == PG_WAIT_LOCK)
            locks++;
    }
    if (out_aas)
        *out_aas = (double)active;
    if (out_lock_fraction)
        *out_lock_fraction = active > 0 ? (double)locks / (double)active : 0.0;
    if (out_cpu_aas)
        *out_cpu_aas = (double)cpu;
}

void pgwt_anomaly_init(struct pgwt_anomaly *a, bool enabled,
                       int sample_rate_hz)
{
    memset(a, 0, sizeof(*a));
    a->enabled       = enabled;
    a->aas_factor    = PGWT_ANOMALY_DEF_AAS_FACTOR;
    a->aas_ticks     = PGWT_ANOMALY_DEF_AAS_TICKS;
    a->lock_fraction = PGWT_ANOMALY_DEF_LOCK_FRAC;
    a->lock_min_aas  = PGWT_ANOMALY_DEF_LOCK_MIN_AAS;
    a->lock_ticks    = PGWT_ANOMALY_DEF_LOCK_TICKS;
    a->cooldown_ns   = (uint64_t)PGWT_ANOMALY_DEF_COOLDOWN_S * 1000000000ULL;
    a->escalation_s  = PGWT_ANOMALY_DEF_ESCALATE_S;
    a->cpu_cusum_enabled = true;
    a->cpu_min_aas   = PGWT_ANOMALY_DEF_CPU_MIN_AAS;
    a->cpu_margin    = PGWT_ANOMALY_DEF_CPU_MARGIN;
    a->cpu_cusum_k   = PGWT_ANOMALY_DEF_CPU_CUSUM_K;
    a->cpu_cusum_h   = PGWT_ANOMALY_DEF_CPU_CUSUM_H;
    a->cpu_cusum_cap = PGWT_ANOMALY_DEF_CPU_CUSUM_CAP;
    a->cpu_armed     = true;
    a->slow_release_div = PGWT_ANOMALY_DEF_SLOW_RELEASE_DIV;

    /* Baseline EWMA: pick alpha so the baseline has roughly a 60-second
     * memory regardless of tick rate. With one update per tick, a half-life
     * of H ticks needs alpha = 1 - 2^(-1/H); approximate with alpha ~ 1/H for
     * the small-alpha regime. H = 60s * rate. Warm up over ~5s of ticks. */
    int hz = sample_rate_hz > 0 ? sample_rate_hz : 10;
    a->sample_period_s = 1.0 / (double)hz;
    double half_life_ticks = 60.0 * (double)hz;
    if (half_life_ticks < 1.0)
        half_life_ticks = 1.0;
    a->baseline_alpha  = 1.0 / half_life_ticks;
    a->warmup_needed   = 5 * hz;        /* ~5 seconds of normal data */
    a->baseline_aas    = 0.0;
    a->baseline_warmup = 0;

    /* ESC-7: continuously-over duration before the baseline starts learning
     * through a sustained regime change (in ticks at this rate). */
    a->learn_through_ticks =
        PGWT_ANOMALY_DEF_LEARN_THROUGH_MIN * 60 * hz;
    if (a->slow_release_div <= 0)
        a->slow_release_div = PGWT_ANOMALY_DEF_SLOW_RELEASE_DIV;
}

struct pgwt_anomaly_decision
pgwt_anomaly_eval_observation(struct pgwt_anomaly *a,
                              const struct pgwt_anomaly_observation *obs,
                              uint64_t now_ns)
{
    struct pgwt_anomaly_decision d;
    memset(&d, 0, sizeof(d));
    d.action        = PGWT_ANOMALY_NONE;
    d.aas           = obs->aas;
    d.lock_fraction = obs->lock_fraction;
    d.cpu_aas       = obs->cpu_aas;
    d.cpu_capacity  = obs->cpu_capacity;
    d.cpu_cusum     = a->cpu_cusum;
    d.baseline      = a->baseline_aas;

    if (!a->enabled)
        return d;

    double aas = obs->aas;
    double lock_fraction = obs->lock_fraction;

    /* Cooldown remains wall-clock based. CPU evidence does not: it uses the
     * nominal sample period below, so one delayed sampler callback is still
     * exactly one observation rather than several seconds of evidence. */
    bool in_cooldown = a->last_fire_ns != 0
                    && now_ns - a->last_fire_ns < a->cooldown_ns;

    /* ── CPU-demand saturation CUSUM ───────────────────────────────────── */
    bool cpu_rule_enabled = a->cpu_cusum_enabled
                         && a->aas_factor < PGWT_ANOMALY_DISABLE_AAS_FACTOR;
    bool cpu_fire = false;
    bool cpu_cusum_climbed = false;

    /* A partial-read tick cannot establish whether demand recovered. Brief
     * gaps hold evidence so intermittent read failures do not erase a real
     * incident. A sustained blind interval can hide genuine recovery, so it
     * invalidates pre-gap evidence. Once a coverage gap starts, UNKNOWN
     * capacity remains part of that blind interval until a trusted return. */
    if (!obs->cpu_coverage_ok
        || (a->cpu_coverage_gap_ticks > 0
            && obs->cpu_capacity <= 0.0)) {
        if (a->cpu_coverage_gap_ticks
            < PGWT_ANOMALY_CPU_COVERAGE_GAP_RESET_TICKS)
            a->cpu_coverage_gap_ticks++;
        if (a->cpu_coverage_gap_ticks
            >= PGWT_ANOMALY_CPU_COVERAGE_GAP_RESET_TICKS)
            a->cpu_cusum = 0.0;
    } else if (obs->cpu_capacity > 0.0) {
        a->cpu_coverage_gap_ticks = 0;
    }

    /* A capacity discontinuity invalidates all evidence accumulated in the
     * old units. An open escalation window or cooldown also clears evidence:
     * this prevents a manual/other-rule window from banking an almost-fire
     * that would immediately consume another budget grant when it closes. */
    if (obs->cpu_capacity_changed || obs->cpu_guard_blocked || in_cooldown) {
        a->cpu_cusum = 0.0;
    } else if (!cpu_rule_enabled) {
        a->cpu_cusum = 0.0;
    } else if (obs->cpu_coverage_ok && obs->cpu_capacity > 0.0) {
        double u = obs->cpu_aas / obs->cpu_capacity;
        if (u < 0.0)
            u = 0.0;
        if (u > a->cpu_cusum_cap)
            u = a->cpu_cusum_cap;

        /* A CPU fire closes the current saturation episode. Until a trusted
         * below-slack tick proves recovery, keep S drained and do not re-arm;
         * continuous saturation therefore consumes at most one grant. */
        if (!a->cpu_armed) {
            a->cpu_cusum = 0.0;
            if (u < a->cpu_cusum_k)
                a->cpu_armed = true;
        } else {
            double reference = a->cpu_cusum_k;
            if (obs->cpu_capacity_affinity_only)
                reference += a->cpu_margin;
            double before = a->cpu_cusum;
            a->cpu_cusum += a->sample_period_s * (u - reference);
            if (a->cpu_cusum < 0.0)
                a->cpu_cusum = 0.0;
            cpu_cusum_climbed = a->cpu_cusum > before;
            cpu_fire = a->cpu_armed
                    && obs->cpu_aas >= a->cpu_min_aas
                    && a->cpu_cusum >= a->cpu_cusum_h;
        }
    }
    /* LOW coverage and UNKNOWN capacity never evaluate demand or fire from an
     * untrusted observation. Brief gaps hold S; sustained gaps clear it above. */
    d.cpu_cusum = a->cpu_cusum;

    /* ── AAS-vs-baseline rule ──────────────────────────────────────────── */
    bool baseline_warm = a->baseline_warmup >= a->warmup_needed;
    bool aas_over = false;
    if (baseline_warm && a->aas_factor > 0.0
        && a->aas_factor < PGWT_ANOMALY_DISABLE_AAS_FACTOR) {
        double threshold = a->aas_factor * a->baseline_aas;
        /* Guard against a near-zero baseline: a tiny absolute AAS over a
         * 0-ish baseline is noise, not an incident. Require at least 2 active
         * sessions before the multiplicative rule can fire. */
        aas_over = (aas >= 2.0) && (aas > threshold);
    }
    if (aas_over)
        a->aas_over_streak++;
    else
        a->aas_over_streak = 0;
    bool aas_fire = aas_over && (a->aas_over_streak >= a->aas_ticks);

    /* ── Lock-class fraction rule (ESC-4: with a min-activity floor) ────── */
    /* Fraction alone fires on a single backend's routine 300 ms row-lock wait
     * (fraction 1.0 of 1 active session), duty-cycling the whole budget away
     * on OLTP noise. Require BOTH a high lock share AND an absolute lock-class
     * AAS floor (lock_aas = fraction * aas) so a lone waiter cannot trip it but
     * a real convoy does — the lock analogue of the AAS rule's aas>=2 floor. */
    double lock_aas = lock_fraction * aas;
    bool lock_over = (a->lock_fraction > 0.0)
                  && (lock_fraction > a->lock_fraction)
                  && (lock_aas >= a->lock_min_aas);
    if (lock_over)
        a->lock_over_streak++;
    else
        a->lock_over_streak = 0;
    bool lock_fire = lock_over && (a->lock_over_streak >= a->lock_ticks);

    /* ── Baseline maintenance ──────────────────────────────────────────── */
    /* Only learn from NORMAL ticks: do not fold an anomalous AAS back into the
     * baseline (that would let a sustained incident silently raise the bar
     * until the rule stops firing). A tick is "normal" for baseline purposes
     * when AAS is not currently over the multiplicative threshold. While
     * warming up we always learn (there is no baseline to protect yet). */
    if (!baseline_warm) {
        /* Simple running mean during warmup so the EWMA starts sane. */
        a->baseline_warmup++;
        double w = (double)a->baseline_warmup;
        a->baseline_aas += (aas - a->baseline_aas) / w;
    } else if (!aas_over) {
        a->baseline_aas += a->baseline_alpha * (aas - a->baseline_aas);
    } else if (a->learn_through_ticks > 0
               && a->aas_over_streak >= a->learn_through_ticks) {
        /* ESC-7: the metric has been continuously over for the sustained-over
         * horizon — treat it as a legitimate regime change and learn through
         * at a reduced rate so the rule stops re-firing forever, without
         * letting a short incident (streak below the horizon) move the bar. */
        int div = a->slow_release_div > 0 ? a->slow_release_div : 1;
        a->baseline_aas += (a->baseline_alpha / (double)div)
                         * (aas - a->baseline_aas);
    }
    d.baseline = a->baseline_aas;

    /* ── Decision ──────────────────────────────────────────────────────── */
    if (!aas_fire && !lock_fire && !cpu_fire) {
        /* Surface a near-trigger if a metric crossed but did not sustain. */
        unsigned near = PGWT_NEAR_NONE;
        if (aas_over && !aas_fire)
            near |= PGWT_NEAR_AAS_SUSTAIN;
        if (lock_over && !lock_fire)
            near |= PGWT_NEAR_LOCK_SUSTAIN;
        if (cpu_cusum_climbed && a->cpu_armed
            && obs->cpu_aas >= a->cpu_min_aas
            && a->cpu_cusum >= 0.5 * a->cpu_cusum_h)
            near |= PGWT_NEAR_CPU_SUSTAIN;
        if (!baseline_warm && aas >= 2.0)
            near |= PGWT_NEAR_BASELINE;
        if (near) {
            d.action = PGWT_ANOMALY_NEAR;
            d.near_mask = near;
            a->near_total++;
        }
        return d;
    }

    /* A rule wants to fire. Enforce cooldown (hysteresis against flapping). */
    unsigned fired = (aas_fire ? PGWT_RULE_AAS : 0)
                   | (lock_fire ? PGWT_RULE_LOCK : 0)
                   | (cpu_fire ? PGWT_RULE_CPU_SATURATION : 0);
    if (in_cooldown) {
        d.action = PGWT_ANOMALY_NEAR;
        d.near_mask = PGWT_NEAR_COOLDOWN;
        d.fired_mask = fired;   /* what WOULD have fired */
        a->near_total++;
        a->dropped_cooldown++;
        return d;
    }

    d.action = PGWT_ANOMALY_FIRE;
    d.fired_mask = fired;
    /* Never carry CPU evidence through any escalation, including a fire
     * caused by another rule on the same tick. */
    a->cpu_cusum = 0.0;
    d.cpu_cusum = 0.0;
    a->last_fire_ns = now_ns;
    a->fires_total++;
    if (fired & PGWT_RULE_CPU_SATURATION) {
        a->cpu_armed = false;
        a->cpu_saturation_fires_total++;
    }
    return d;
}

struct pgwt_anomaly_decision
pgwt_anomaly_eval(struct pgwt_anomaly *a, double aas, double lock_fraction,
                  uint64_t now_ns)
{
    const struct pgwt_anomaly_observation obs = {
        .aas = aas,
        .lock_fraction = lock_fraction,
        .cpu_capacity = -1.0,
        .cpu_coverage_ok = false,
    };
    return pgwt_anomaly_eval_observation(a, &obs, now_ns);
}

/* ── Daemon-side wrapper ──────────────────────────────────────────────── */

#ifndef PGWT_SERVER

#include "daemon.h"
#include "effective_cores.h"
#include "escalation.h"
#include "sampler.h"

#include <time.h>

static uint64_t anomaly_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static const char *rule_names(unsigned mask, char *buf, size_t len)
{
    buf[0] = '\0';
    if (mask & PGWT_RULE_AAS)
        snprintf(buf + strlen(buf), len - strlen(buf), "aas ");
    if (mask & PGWT_RULE_LOCK)
        snprintf(buf + strlen(buf), len - strlen(buf), "lock ");
    if (mask & PGWT_RULE_CPU_SATURATION)
        snprintf(buf + strlen(buf), len - strlen(buf), "cpu-saturation ");
    if (buf[0] == '\0')
        snprintf(buf, len, "(none)");
    return buf;
}

void pgwt_anomaly_tick(struct pgwt_daemon *d,
                       const struct pgwt_trace_event *samples, int n)
{
    struct pgwt_anomaly *a = &d->anomaly;
    if (!a->enabled)
        return;

    /* Stage 2 capacity is refreshed at the same cadence as CPU evidence.
     * materially_changed is therefore consumed on the exact sampler tick that
     * resets S, rather than being lost to the slower display timer. */
    d->effective_cores = pgwt_effective_cores_refresh(
        &d->cpu_capacity_resolver, d->postmaster_pid);

    double aas = 0.0, lock_fraction = 0.0, cpu_aas = 0.0;
    pgwt_anomaly_metrics_from_batch(samples, n, &aas, &lock_fraction,
                                    &cpu_aas);

    /* Complete read coverage is the conservative evidence boundary. Missing
     * targets are omitted from the batch; holding S avoids treating that
     * partial count as a trustworthy saturation observation. A legitimate
     * empty registry (0 targets, 0 invalid) remains a valid zero-demand tick. */
    bool coverage_ok = d->sampler == NULL
                    || d->sampler->read_invalid_last == 0;
    struct pgwt_anomaly_observation obs = {
        .aas = aas,
        .lock_fraction = lock_fraction,
        .cpu_aas = cpu_aas,
        .cpu_capacity = d->effective_cores.cores,
        .cpu_capacity_affinity_only =
            d->effective_cores.source == PGWT_EFFECTIVE_CORES_AFFINITY,
        .cpu_coverage_ok = coverage_ok,
        .cpu_capacity_changed = d->effective_cores.materially_changed,
        .cpu_guard_blocked = d->escalation.active,
    };

    uint64_t now = anomaly_mono_ns();
    struct pgwt_anomaly_decision dec =
        pgwt_anomaly_eval_observation(a, &obs, now);

    char rb[32];
    switch (dec.action) {
    case PGWT_ANOMALY_NONE:
        /* A quiet tick ends the near run; the next near-trigger, even with the
         * same reason mask, is fresh news and logs immediately (ESC-8). */
        a->last_near_mask = PGWT_NEAR_NONE;
        break;

    case PGWT_ANOMALY_NEAR: {
        /* ESC-8: near-triggers can recur every tick (up to 10 lines/s) — that
         * floods the log while telling the operator nothing new. Rate-limit to
         * one line per CHANGE of the near reason mask, plus a periodic summary
         * (with a suppressed-count) at most once a minute, so the tuning value
         * of the data is kept (near_total already counts every one for the
         * control socket) without the flood. */
        const uint64_t NEAR_SUMMARY_NS = 60ULL * 1000000000ULL;
        bool mask_changed = (dec.near_mask != a->last_near_mask);
        bool summary_due = (a->last_near_log_ns == 0)
                        || (now - a->last_near_log_ns >= NEAR_SUMMARY_NS);
        if (mask_changed || summary_due) {
            char since[48] = "";
            if (a->near_since_log > 0)
                snprintf(since, sizeof(since), " (+%llu suppressed)",
                         (unsigned long long)a->near_since_log);
            fprintf(stderr,
                    "INFO: anomaly near-trigger: aas=%.1f baseline=%.2f "
                    "lock_frac=%.2f cpu_aas=%.1f cpu_cusum=%.3f%s%s%s%s%s%s\n",
                    dec.aas, dec.baseline, dec.lock_fraction,
                    dec.cpu_aas, dec.cpu_cusum,
                    (dec.near_mask & PGWT_NEAR_AAS_SUSTAIN) ? " [aas-sustain]" : "",
                    (dec.near_mask & PGWT_NEAR_LOCK_SUSTAIN) ? " [lock-sustain]" : "",
                    (dec.near_mask & PGWT_NEAR_COOLDOWN) ? " [cooldown]" : "",
                    (dec.near_mask & PGWT_NEAR_BASELINE) ? " [baseline-warmup]" : "",
                    (dec.near_mask & PGWT_NEAR_CPU_SUSTAIN) ? " [cpu-sustain]" : "",
                    since);
            a->last_near_mask = dec.near_mask;
            a->last_near_log_ns = now;
            a->near_since_log = 0;
        } else {
            a->near_since_log++;
        }
        break;
    }

    case PGWT_ANOMALY_FIRE: {
        int granted = 0;
        const char *why = NULL;
        rule_names(dec.fired_mask, rb, sizeof(rb));
        int reason = (dec.fired_mask & PGWT_RULE_CPU_SATURATION)
                   ? PGWT_ESC_REASON_CPU_SATURATION
                   : PGWT_ESC_REASON_ANOMALY;
        int rc = pgwt_escalate(d, a->escalation_s, reason,
                               &granted, &why);
        if (rc == 0) {
            fprintf(stderr,
                    "INFO: anomaly AUTO-escalation: rule=%saas=%.1f "
                    "baseline=%.2f lock_frac=%.2f cpu_aas=%.1f "
                    "capacity=%.3f -> full fidelity %ds\n",
                    rb, dec.aas, dec.baseline, dec.lock_fraction,
                    dec.cpu_aas, dec.cpu_capacity, granted);
        } else {
            /* Over budget: dropped SILENTLY (log only, no error to anyone) —
             * unlike a manual escalate which returns a denial to its caller. */
            a->dropped_budget++;
            fprintf(stderr,
                    "INFO: anomaly trigger DROPPED (budget): rule=%saas=%.1f "
                    "lock_frac=%.2f (%s)\n",
                    rb, dec.aas, dec.lock_fraction,
                    why ? why : "budget exhausted");
        }
        break;
    }
    }
}

#endif /* !PGWT_SERVER */
