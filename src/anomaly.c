/* anomaly.c — Anomaly-triggered escalation rules (Track A, D5 / Phase A5)
 *
 * See anomaly.h for the design. The pure rule core (pgwt_anomaly_eval and
 * pgwt_anomaly_metrics_from_batch) is free of BPF and the escalation engine so
 * the unit test (tests/test_anomaly.c) can drive it with scripted sample
 * streams. The daemon-side wrapper (pgwt_anomaly_tick) connects the FIRE
 * decision to A4's pgwt_escalate() and logs every near-trigger.
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

static double max_double(double x, double y)
{
    return x > y ? x : y;
}

static double min_double(double x, double y)
{
    return x < y ? x : y;
}

static double abs_double(double x)
{
    return x < 0.0 ? -x : x;
}

void pgwt_anomaly_metrics_from_batch(const struct pgwt_trace_event *samples,
                                     int n, double *out_aas,
                                     double *out_lock_fraction)
{
    int active = 0;
    int locks = 0;
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
        if (WE_CLASS(we) == PG_WAIT_LOCK)
            locks++;
    }
    if (out_aas)
        *out_aas = (double)active;
    if (out_lock_fraction)
        *out_lock_fraction = active > 0 ? (double)locks / (double)active : 0.0;
}

void pgwt_anomaly_init(struct pgwt_anomaly *a, bool enabled,
                       int sample_rate_hz)
{
    memset(a, 0, sizeof(*a));
    a->enabled       = enabled;
    a->aas_factor    = PGWT_ANOMALY_DEF_AAS_FACTOR;
    a->aas_ticks     = PGWT_ANOMALY_DEF_AAS_TICKS;
    a->dev_k         = PGWT_ANOMALY_DEF_DEV_K;
    a->mad_floor_abs = PGWT_ANOMALY_DEF_MAD_FLOOR_ABS;
    a->mad_floor_frac = PGWT_ANOMALY_DEF_MAD_FLOOR_FRAC;
    a->dev_aas_floor = PGWT_ANOMALY_DEF_DEV_AAS_FLOOR;
    a->dev_ticks     = PGWT_ANOMALY_DEF_DEV_TICKS;
    a->lock_fraction = PGWT_ANOMALY_DEF_LOCK_FRAC;
    a->lock_min_aas  = PGWT_ANOMALY_DEF_LOCK_MIN_AAS;
    a->lock_ticks    = PGWT_ANOMALY_DEF_LOCK_TICKS;
    a->cooldown_ns   = (uint64_t)PGWT_ANOMALY_DEF_COOLDOWN_S * 1000000000ULL;
    a->escalation_s  = PGWT_ANOMALY_DEF_ESCALATE_S;
    a->slow_release_div = PGWT_ANOMALY_DEF_SLOW_RELEASE_DIV;

    /* Baseline EWMA: pick alpha so the baseline has roughly a 60-second
     * memory regardless of tick rate. With one update per tick, a half-life
     * of H ticks needs alpha = 1 - 2^(-1/H); approximate with alpha ~ 1/H for
     * the small-alpha regime. H = 60s * rate. Warm up over ~5s of ticks. */
    int hz = sample_rate_hz > 0 ? sample_rate_hz : 10;
    double half_life_ticks = 60.0 * (double)hz;
    if (half_life_ticks < 1.0)
        half_life_ticks = 1.0;
    a->baseline_alpha  = 1.0 / half_life_ticks;
    a->warmup_needed   = 5 * hz;        /* ~5 seconds of normal data */
    a->baseline_aas    = 0.0;
    a->baseline_warmup = 0;
    a->mad_aas         = 0.0;
    a->dev_maturity_ticks = PGWT_ANOMALY_DEF_DEV_MATURITY_S * hz;

    /* ESC-7: continuously-over duration before the baseline starts learning
     * through a sustained regime change (in ticks at this rate). */
    a->learn_through_ticks =
        PGWT_ANOMALY_DEF_LEARN_THROUGH_MIN * 60 * hz;
    if (a->slow_release_div <= 0)
        a->slow_release_div = PGWT_ANOMALY_DEF_SLOW_RELEASE_DIV;
}

struct pgwt_anomaly_decision
pgwt_anomaly_eval(struct pgwt_anomaly *a, double aas, double lock_fraction,
                  uint64_t now_ns)
{
    struct pgwt_anomaly_decision d;
    memset(&d, 0, sizeof(d));
    d.action        = PGWT_ANOMALY_NONE;
    d.aas           = aas;
    d.lock_fraction = lock_fraction;
    d.baseline      = a->baseline_aas;
    d.mad           = a->mad_aas;

    if (!a->enabled)
        return d;

    if (a->ticks_observed < INT32_MAX)
        a->ticks_observed++;

    /* ── AAS-vs-baseline rule ──────────────────────────────────────────── */
    bool baseline_warm = a->baseline_warmup >= a->warmup_needed;
    bool aas_rule_enabled = a->aas_factor > 0.0
                         && a->aas_factor < PGWT_ANOMALY_AAS_DISABLE_FACTOR;
    bool dev_rule_enabled = aas_rule_enabled
                         && a->dev_k > 0.0
                         && a->dev_ticks > 0
                         && a->dev_aas_floor > 0.0;
    bool dev_mature = a->dev_maturity_ticks <= 0
                   || a->ticks_observed >= a->dev_maturity_ticks;
    bool primary_over = false;
    bool deviation_over = false;
    if (baseline_warm && aas_rule_enabled) {
        double threshold = a->aas_factor * a->baseline_aas;
        /* Guard against a near-zero baseline: a tiny absolute AAS over a
         * 0-ish baseline is noise, not an incident. Require at least 2 active
         * sessions before the multiplicative rule can fire. During the robust
         * estimator's startup only, also require its baseline to have reached
         * the point where the primary threshold itself is at least that floor.
         * This rejects the S6 post-lull 0.5 -> 2.0 transition without delaying
         * a classic idle/zero -> 4.0 primary fire: load above the absolute
         * floor always keeps the shipped fast path. */
        bool primary_ready = !dev_rule_enabled || dev_mature
                          || aas > 2.0
                          || a->baseline_aas >= 2.0 / a->aas_factor;
        primary_over = primary_ready && (aas >= 2.0) && (aas > threshold);

        if (dev_rule_enabled && dev_mature) {
            double mad_floor = max_double(a->mad_floor_abs,
                                           a->mad_floor_frac * a->baseline_aas);
            double scale = max_double(a->mad_aas, mad_floor);
            deviation_over = aas >= a->dev_aas_floor
                          && aas > a->baseline_aas + a->dev_k * scale;
        }
    }

    bool aas_over = primary_over || deviation_over;
    if (aas_over)
        a->aas_over_streak++;
    else
        a->aas_over_streak = 0;
    if (primary_over)
        a->primary_over_streak++;
    else
        a->primary_over_streak = 0;
    if (deviation_over)
        a->dev_over_streak++;
    else
        a->dev_over_streak = 0;

    bool primary_fire = primary_over
                     && a->primary_over_streak >= a->aas_ticks;
    bool deviation_fire = deviation_over
                       && a->dev_over_streak >= a->dev_ticks;
    bool aas_fire = primary_fire || deviation_fire;

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
    /* Warmup uses running means. Afterwards the robust path learns baseline
     * and MAD every tick, but clips each residual to three current robust
     * scales before folding it in. A single incident therefore has bounded
     * influence while recurring structure is eventually absorbed. */
    if (!baseline_warm) {
        a->baseline_warmup++;
        double w = (double)a->baseline_warmup;
        a->baseline_aas += (aas - a->baseline_aas) / w;
        double dev = abs_double(aas - a->baseline_aas);
        a->mad_warmup++;
        double mw = (double)a->mad_warmup;
        a->mad_aas += (dev - a->mad_aas) / mw;
    } else if (dev_rule_enabled) {
        double mad_floor = max_double(a->mad_floor_abs,
                                       a->mad_floor_frac * a->baseline_aas);
        double scale = max_double(a->mad_aas, mad_floor);
        double residual = aas - a->baseline_aas;
        double cap = PGWT_ANOMALY_MAD_WINSOR_K * scale;
        double clipped = max_double(-cap, min_double(residual, cap));
        double alpha = a->baseline_alpha;

        /* Retain ESC-7's deliberately slow release after a continuously high
         * primary regime. The robust path already bounds every earlier tick;
         * crossing the legacy learn-through horizon must not speed it up. */
        if (primary_over && a->learn_through_ticks > 0
            && a->primary_over_streak >= a->learn_through_ticks) {
            int div = a->slow_release_div > 0 ? a->slow_release_div : 1;
            alpha /= (double)div;
        }
        a->baseline_aas += alpha * clipped;
        double abs_clipped = min_double(abs_double(residual), cap);
        a->mad_aas += a->baseline_alpha * (abs_clipped - a->mad_aas);
    } else if (!primary_over) {
        /* Independent deviation off-switch restores the shipped primary
         * baseline semantics exactly. */
        a->baseline_aas += a->baseline_alpha * (aas - a->baseline_aas);
    } else if (a->learn_through_ticks > 0
               && a->primary_over_streak >= a->learn_through_ticks) {
        int div = a->slow_release_div > 0 ? a->slow_release_div : 1;
        a->baseline_aas += (a->baseline_alpha / (double)div)
                         * (aas - a->baseline_aas);
    }
    d.baseline = a->baseline_aas;
    d.mad = a->mad_aas;

    /* ── Decision ──────────────────────────────────────────────────────── */
    if (!aas_fire && !lock_fire) {
        /* Surface a near-trigger if a metric crossed but did not sustain. */
        unsigned near = PGWT_NEAR_NONE;
        if (primary_over && !primary_fire)
            near |= PGWT_NEAR_AAS_SUSTAIN;
        if (deviation_over && !deviation_fire)
            near |= PGWT_NEAR_DEV_SUSTAIN;
        if (lock_over && !lock_fire)
            near |= PGWT_NEAR_LOCK_SUSTAIN;
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
                   | (lock_fire ? PGWT_RULE_LOCK : 0);
    if (a->last_fire_ns != 0 &&
        now_ns - a->last_fire_ns < a->cooldown_ns) {
        d.action = PGWT_ANOMALY_NEAR;
        d.near_mask = PGWT_NEAR_COOLDOWN;
        d.fired_mask = fired;   /* what WOULD have fired */
        a->near_total++;
        a->dropped_cooldown++;
        return d;
    }

    d.action = PGWT_ANOMALY_FIRE;
    d.fired_mask = fired;
    a->last_fire_ns = now_ns;
    a->fires_total++;
    return d;
}

/* ── Daemon-side wrapper ──────────────────────────────────────────────── */

#ifndef PGWT_SERVER

#include "daemon.h"
#include "escalation.h"

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

    double aas = 0.0, lock_fraction = 0.0;
    pgwt_anomaly_metrics_from_batch(samples, n, &aas, &lock_fraction);

    uint64_t now = anomaly_mono_ns();
    struct pgwt_anomaly_decision dec =
        pgwt_anomaly_eval(a, aas, lock_fraction, now);

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
                    "mad=%.2f lock_frac=%.2f%s%s%s%s%s%s\n",
                    dec.aas, dec.baseline, dec.mad, dec.lock_fraction,
                    (dec.near_mask & PGWT_NEAR_AAS_SUSTAIN) ? " [aas-sustain]" : "",
                    (dec.near_mask & PGWT_NEAR_DEV_SUSTAIN) ? " [dev-sustain]" : "",
                    (dec.near_mask & PGWT_NEAR_LOCK_SUSTAIN) ? " [lock-sustain]" : "",
                    (dec.near_mask & PGWT_NEAR_COOLDOWN) ? " [cooldown]" : "",
                    (dec.near_mask & PGWT_NEAR_BASELINE) ? " [baseline-warmup]" : "",
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
        int rc = pgwt_escalate(d, a->escalation_s, PGWT_ESC_REASON_ANOMALY,
                               &granted, &why);
        if (rc == 0) {
            fprintf(stderr,
                    "INFO: anomaly AUTO-escalation: rule=%saas=%.1f "
                    "baseline=%.2f mad=%.2f lock_frac=%.2f -> full fidelity %ds\n",
                    rb, dec.aas, dec.baseline, dec.mad,
                    dec.lock_fraction, granted);
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
