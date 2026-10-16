/* control.c — Daemon control socket: unix socket + JSON-line protocol */
#define _GNU_SOURCE   /* accept4 */
#include "control.h"
#include "daemon.h"
#include "event_writer.h"
#include "provider.h"
#include "escalation.h"
#include "sampler.h"
#include "map_reader.h"
#include "open_snapshot.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/epoll.h>

/* ── Helpers ──────────────────────────────────────────────── */

static uint64_t now_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Add uint64 as raw JSON number (avoids double precision loss) */
static void cjson_add_uint64(cJSON *obj, const char *name, uint64_t val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)val);
    cJSON_AddRawToObject(obj, name, buf);
}

/* Lowercase token for an escalation reason ("manual"|"anomaly"|...). Used so
 * the UI can distinguish WHY a full-fidelity window is open (e.g. annotate a
 * manual escalate differently from an anomaly-triggered one). */
static const char *esc_reason_str(int reason)
{
    switch (reason) {
    case PGWT_ESC_REASON_MANUAL:   return "manual";
    case PGWT_ESC_REASON_ANOMALY:  return "anomaly";
    case PGWT_ESC_REASON_EXPIRED:  return "expired";
    case PGWT_ESC_REASON_REQUEST:  return "request";
    case PGWT_ESC_REASON_SHUTDOWN: return "shutdown";
    case PGWT_ESC_REASON_BUDGET:   return "budget";
    case PGWT_ESC_REASON_CPU_SATURATION: return "cpu_saturation";
    default:                       return "unknown";
    }
}

/* The capture tier being recorded RIGHT NOW, honest for every mode (ESC-9):
 * tiered flips sampled/escalated; a fixed EXACT-fidelity provider (--mode full)
 * is always "escalated" (full fidelity); everything else is "sampled". Shared
 * by status and metrics so they can never disagree. */
static const char *daemon_tier_str(const struct pgwt_daemon *d)
{
    if (d->escalation.enabled)
        return d->escalation.active ? "escalated" : "sampled";
    if (d->provider && d->provider->fidelity == PGWT_FIDELITY_EXACT)
        return "escalated";   /* full mode: always full fidelity */
    return "sampled";
}

/* Count live tracked backends */
static int count_backends(const struct pgwt_daemon *d)
{
    int n = 0;
    for (int i = 0; i < d->backends.count; i++)
        if (d->backends.entries[i].is_alive)
            n++;
    return n;
}

/* ── Response builders ────────────────────────────────────── */

static cJSON *build_status(const struct pgwt_daemon *d)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", 1);
    /* Capture tier. --lightweight is reported as its own mode so the answer
     * is never a lie about what is being captured; otherwise report the
     * active provider name ("full" | "sampled"), with tiered distinguished. */
    const char *mode;
    if (d->lightweight_mode)
        mode = "lightweight";
    else if (d->mode == PGWT_MODE_TIERED)
        mode = "tiered";
    else if (d->provider)
        mode = d->provider->name;
    else
        mode = "full";
    cJSON_AddStringToObject(root, "mode", mode);
    cJSON_AddNumberToObject(root, "uptime_s",
                            (double)(now_mono_ns() - d->start_ts) / 1e9);

    /* AAS-1 Stage 2: target postmaster capacity, refreshed periodically.
     * UNKNOWN is the explicit -1 sentinel; it is never replaced by a guess. */
    cJSON_AddNumberToObject(root, "effective_cpu_capacity_cores",
                            d->effective_cores.cores);
    cJSON_AddStringToObject(root, "effective_cpu_capacity_source",
                            pgwt_effective_cores_source_name(
                                d->effective_cores.source));
    cJSON_AddNumberToObject(root, "backends", count_backends(d));
    cJSON_AddNumberToObject(root, "pg_pid", d->postmaster_pid);
    cJSON_AddStringToObject(root, "version", PGWT_BUILD_VERSION);
    cJSON_AddStringToObject(root, "pgbackend_layout_source",
                            pgwt_pgbs_source_name(
                                d->backend_status_layout.source));
    cJSON_AddStringToObject(root, "pgbackend_layout_validation",
                            pgwt_pgbs_validation_name(
                                d->backend_status_layout.validation));
    cJSON_AddNumberToObject(root, "pgbackend_layout_fallback_fields",
                            pgwt_pgbs_fallback_count(
                                &d->backend_status_layout));
    cJSON_AddStringToObject(root, "sampled_attr_source",
                            pgwt_pgbs_sampled_attr_enabled(
                                &d->backend_status_layout)
                                ? "pgbackend_status"
                                : "uprobe_state_map");

    /* Current capture tier and escalation state (A4). "tier" is what is being
     * captured right now: in tiered mode it flips between "sampled" (always-on
     * baseline) and "escalated" (full-fidelity window open); for full/sampled
     * it mirrors the fixed provider fidelity. */
    cJSON_AddStringToObject(root, "tier", daemon_tier_str(d));
    cJSON_AddBoolToObject(root, "escalation_supported", d->escalation.enabled);
    cjson_add_uint64(root, "exact_probe_generation",
                     d->escalation.generation);
    cJSON_AddNumberToObject(root, "exact_probe_attached_mask",
                            d->exact_probes.core.attached_mask);
    cJSON_AddBoolToObject(root, "exact_probe_warmup_reconciled",
                          d->exact_probes.sampled_warmup_reconciled);
    cJSON_AddBoolToObject(root, "exact_cpu_active", d->exact_cpu_active);
    cjson_add_uint64(root, "exact_query_uprobe_fires_total",
                     pgwt_exact_uprobe_fire_count(
                         (struct pgwt_daemon *)d, PGWT_UPROBE_FIRE_QUERY));
    cjson_add_uint64(root, "exact_activity_uprobe_fires_total",
                     pgwt_exact_uprobe_fire_count(
                         (struct pgwt_daemon *)d, PGWT_UPROBE_FIRE_ACTIVITY));
    cJSON_AddNumberToObject(root, "escalation_seconds_remaining",
                            pgwt_escalation_remaining_s(d));
    cJSON_AddNumberToObject(root, "escalation_budget_remaining_s",
                            pgwt_escalation_budget_remaining_s(d));
    /* ESC-6: report the budget mode truthfully. unlimited => remaining is the
     * -1 sentinel; this flag disambiguates it from a finite budget. */
    cJSON_AddBoolToObject(root, "escalation_budget_unlimited",
                          d->escalation.budget_unlimited);
    /* Reason of the currently-open window (so the UI can annotate manual vs
     * anomaly escalations distinctly). "none" while not escalated. */
    cJSON_AddStringToObject(root, "escalation_reason",
                            d->escalation.active
                                ? esc_reason_str(d->escalation.window_reason)
                                : "none");

    /* Sampler read health (SMP-1). A total read failure renders as an idle
     * database in the DATA — this out-of-band flag is how a client can tell
     * "idle" from "blind". healthy=true when no sampler runs (full mode). */
    bool sampler_healthy = true;
    char reason[160] = "";
    if (d->sampler && !d->sampler->health.healthy) {
        sampler_healthy = false;
        snprintf(reason, sizeof(reason),
                 "sampler reads failing: %s (%llu consecutive ticks, "
                 "%llu total)",
                 strerror(d->sampler->health.last_errno),
                 (unsigned long long)d->sampler->health.consec_failed_ticks,
                 (unsigned long long)d->sampler->health.failed_ticks_total);
    }
    cJSON_AddBoolToObject(root, "sampler_healthy", sampler_healthy);
    cJSON_AddStringToObject(root, "sampler_unhealthy_reason", reason);

    /* T8 measured-CPU capability (docs/ROADMAP_AND_STATUS.md), reported
     * as loudly here as in the startup log — never silent. "measured": the
     * exact tier reads task se.sum_exec_runtime deltas and the trace carries
     * real cpu_ns + Off-CPU*. "legacy": /proc schedstat was unavailable
     * (CONFIG_SCHED_INFO off), so CPU is inferred from wait gaps (includes
     * runqueue/throttle time) and cpu_ns carries the UNKNOWN sentinel. */
    cJSON_AddStringToObject(root, "cpu_accounting",
                            d->cpu_accounting ? "measured" : "legacy");
    /* #98: how the live view classifies a client backend's on-CPU
     * intervals — "markers" (CMD_START/CMD_END sweep, the server's majority
     * rule) or "ungated" (gate unavailable: all client we==0 is CPU*). */
    cJSON_AddStringToObject(root, "live_cpu_gate", pgwt_live_cpu_gate_name(d));
    return root;
}

static cJSON *build_metrics(const struct pgwt_daemon *d)
{
    const struct pgwt_counters *ctr = &d->counters;
    struct pgwt_metrics pm;
    memset(&pm, 0, sizeof(pm));
    if (d->provider && d->provider->self_metrics)
        d->provider->self_metrics((struct pgwt_daemon *)d, &pm);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON_AddNumberToObject(root, "uptime_s",
                            (double)(now_mono_ns() - d->start_ts) / 1e9);

    /* AAS-1 Stage 2: target postmaster capacity, refreshed periodically.
     * UNKNOWN is the explicit -1 sentinel; it is never replaced by a guess. */
    cJSON_AddNumberToObject(root, "effective_cpu_capacity_cores",
                            d->effective_cores.cores);
    cJSON_AddStringToObject(root, "effective_cpu_capacity_source",
                            pgwt_effective_cores_source_name(
                                d->effective_cores.source));

    /* Event path counters (incremented in event_stream.c / daemon.c) */
    cjson_add_uint64(root, "events_total", ctr->events_total);
    cJSON_AddNumberToObject(root, "events_per_sec", ctr->events_per_sec);
    cjson_add_uint64(root, "lifecycle_events_total",
                     ctr->lifecycle_events_total);

    /* Watchpoint attach failures (incremented in backend.c) */
    cjson_add_uint64(root, "wp_attach_failures_total",
                     ctr->wp_attach_failures_total);

    cJSON_AddNumberToObject(root, "backends_tracked", count_backends(d));

    /* Sampled tier counters (0 when not sampling) */
    cjson_add_uint64(root, "samples_total", ctr->samples_total);
    cJSON_AddNumberToObject(root, "samples_per_sec", ctr->samples_per_sec);
    cjson_add_uint64(root, "sample_read_faults_total",
                     ctr->sample_read_faults_total);
    cjson_add_uint64(root, "pgbackend_layout_fallbacks_total",
                     ctr->pgbackend_layout_fallbacks_total);
    cjson_add_uint64(root, "sampled_attr_tick_read_failures_total",
                     ctr->sampled_attr_tick_read_failures_total);
    /* SCOPE (#294), repeated here because a metrics scrape sees these keys and
     * not daemon.h: every sampled_attr_shadow_* key compares the TARGET-LOOP
     * status read against the uprobe map, i.e. the read taken BEFORE the
     * batched wait_event_info read and so BEFORE the #294 command-gate
     * recheck, whose result OVERRIDES that cmd_open for on-CPU samples.
     * sampled_attr_shadow_cmd_open_mismatch_total therefore measures the
     * PRE-RECHECK read, not the gate the sampler actually shipped; it is
     * unchanged by the #294 fix by construction. For the gate in force use
     * cmd_gate_order_recovered_total / cmd_gate_order_at_risk_total. */
    cjson_add_uint64(root, "sampled_attr_shadow_total",
                     ctr->sampled_attr_shadow_total);
    cjson_add_uint64(root, "sampled_attr_shadow_mismatch_total",
                     ctr->sampled_attr_shadow_mismatch_total);
    cjson_add_uint64(root, "sampled_attr_shadow_active_total",
                     ctr->sampled_attr_shadow_active_total);
    cjson_add_uint64(root, "sampled_attr_shadow_active_mismatch_total",
                     ctr->sampled_attr_shadow_active_mismatch_total);
    cjson_add_uint64(root, "sampled_attr_shadow_cmd_open_mismatch_total",
                     ctr->sampled_attr_shadow_cmd_open_mismatch_total);
    cjson_add_uint64(root, "sampled_attr_shadow_query_id_mismatch_total",
                     ctr->sampled_attr_shadow_query_id_mismatch_total);
    cjson_add_uint64(root, "sampled_text_pending",
                     __atomic_load_n(&ctr->sampled_text_pending,
                                     __ATOMIC_RELAXED));
    cjson_add_uint64(root, "sampled_text_resolved_total",
                     __atomic_load_n(&ctr->sampled_text_resolved_total,
                                     __ATOMIC_RELAXED));
    cjson_add_uint64(root, "sampled_text_absent_total",
                     __atomic_load_n(&ctr->sampled_text_absent_total,
                                     __ATOMIC_RELAXED));
    cjson_add_uint64(root, "sampled_text_evicted_total",
                     __atomic_load_n(&ctr->sampled_text_evicted_total,
                                     __ATOMIC_RELAXED));
    cjson_add_uint64(root, "sampled_text_error_total",
                     __atomic_load_n(&ctr->sampled_text_error_total,
                                     __ATOMIC_RELAXED));
    cjson_add_uint64(root, "sampled_text_retry_exhausted_total",
                     __atomic_load_n(&ctr->sampled_text_retry_exhausted_total,
                                     __ATOMIC_RELAXED));
    cjson_add_uint64(root, "sampled_text_pgss_scans_total",
                     __atomic_load_n(&ctr->sampled_text_pgss_scans_total,
                                     __ATOMIC_RELAXED));
    cjson_add_uint64(root, "exact_query_uprobe_fires_total",
                     pgwt_exact_uprobe_fire_count(
                         (struct pgwt_daemon *)d, PGWT_UPROBE_FIRE_QUERY));
    cjson_add_uint64(root, "exact_activity_uprobe_fires_total",
                     pgwt_exact_uprobe_fire_count(
                         (struct pgwt_daemon *)d, PGWT_UPROBE_FIRE_ACTIVITY));
    cJSON_AddNumberToObject(root, "exact_probe_attached_mask",
                            d->exact_probes.core.attached_mask);
    cJSON_AddBoolToObject(root, "exact_probe_warmup_reconciled",
                          d->exact_probes.sampled_warmup_reconciled);
    cJSON_AddBoolToObject(root, "exact_cpu_active", d->exact_cpu_active);
    cjson_add_uint64(root, "exact_probe_generation",
                     d->escalation.generation);
    cJSON_AddNumberToObject(root, "exact_preseed_entries",
                            pgwt_exact_seed_count((struct pgwt_daemon *)d,
                                                  d->escalation.generation));
    cjson_add_uint64(root, "sample_read_failures_total",
                     pm.sample_read_failures_total);
    cJSON_AddNumberToObject(root, "sample_read_targets",
                            pm.sample_read_targets);
    cJSON_AddNumberToObject(root, "sample_read_valid",
                            pm.sample_read_valid);
    cJSON_AddNumberToObject(root, "sample_read_invalid",
                            pm.sample_read_invalid);
    cJSON_AddNumberToObject(root, "sample_read_coverage_pct",
                            pm.sample_read_targets > 0
                                ? 100.0 * (double)pm.sample_read_valid
                                  / (double)pm.sample_read_targets
                                : 0.0);
    cjson_add_uint64(root, "sampler_ticks_missed_total",
                     ctr->sampler_ticks_missed_total);

    /* Capture hardening (T4): any non-zero here is a loudly-logged problem.
     * state_map_full_total = BPF-side insert failures + userspace
     * preseed/seed failures (each one is a backend recording nothing or
     * losing query attribution — CAP-1). seen_query_ids_full_total = FULL/raw
     * first-seen metadata insert failures only; sampled resolver activity does
     * not touch it. invalid_wait_reads_total =
     * garbage class-byte readings dropped (wrong offset backstop, CAP-2/5).
     * state_reseeds_total = frozen-stale entries reseeded by the per-tick
     * sweep (seed→arm race repair, one INFO line each). */
    cjson_add_uint64(root, "state_map_full_total",
                     ctr->state_map_full_total
                     + pgwt_read_bpf_fail_counter((struct pgwt_daemon *)d,
                                                  PGWT_BPF_FAIL_STATE_MAP));
    cjson_add_uint64(root, "seen_query_ids_full_total",
                     pgwt_read_bpf_fail_counter((struct pgwt_daemon *)d,
                                                PGWT_BPF_FAIL_SEEN_QIDS));
    cjson_add_uint64(root, "invalid_wait_reads_total",
                     ctr->invalid_wait_reads_total);
    cjson_add_uint64(root, "state_reseeds_total",
                     ctr->state_reseeds_total);
    cJSON_AddBoolToObject(root, "sampler_healthy",
                          !(d->sampler && !d->sampler->health.healthy));

    /* T2 decomposed-AAS observability (docs/AAS_SEMANTICS_DECISION.md):
     * io_worker_busy_pct is the utilization signal that replaces counting
     * io_workers in AAS (busy% over the last display interval; a sustained
     * high value means the io_worker pool is near saturation).
     * noncmd_cpu_samples_total counts client we==0 readings outside a
     * command — observed but deliberately not recorded as activity. */
    cJSON_AddNumberToObject(root, "io_worker_busy_pct",
                            ctr->io_worker_busy_pct);

    /* T8 measured-CPU counters (docs/ROADMAP_AND_STATUS.md). Lifetime
     * totals over exact-tier measured intervals; all 0 on a legacy daemon.
     * cpu_accounting mirrors status so a metrics scrape alone reveals the tier.
     * wait_gap_cpu_ns_total is the self-check (should stay ≈0 vs total wait). */
    cJSON_AddStringToObject(root, "cpu_accounting",
                            d->cpu_accounting ? "measured" : "legacy");
    cjson_add_uint64(root, "cpu_ns_total", ctr->cpu_ns_total);
    cjson_add_uint64(root, "offcpu_ns_total", ctr->offcpu_ns_total);
    cjson_add_uint64(root, "cpu_clamped_total", ctr->cpu_clamped_ns_total);
    cjson_add_uint64(root, "wait_gap_cpu_ns_total", ctr->wait_gap_cpu_ns_total);
    /* #97 multi-window fail-safe: windowed-delta fields clamped at 0 instead
     * of wrapping (an open stretch closed under a different label). */
    cjson_add_uint64(root, "ring_delta_clamps_total",
                     ctr->ring_delta_clamps_total);
    /* #202: the over-attribution direction, which the clamp counter above
     * is structurally blind to (it only sees a field going DOWN). */
    cjson_add_uint64(root, "ring_delta_overshoot_ns_total",
                     ctr->ring_delta_overshoot_ns_total);
    /* #98 live command gate: how the live CPU* classification was decided.
     * live_cpu_gate mirrors status; the counters are the non-marker share
     * (unmarked pids / full accumulator), so a scrape alone tells whether
     * the live CPU* row is marker-classified or falling back. */
    cJSON_AddStringToObject(root, "live_cpu_gate", pgwt_live_cpu_gate_name(d));
    cjson_add_uint64(root, "live_cmd_markers_total",
                     ctr->live_cmd_markers_total);
    cjson_add_uint64(root, "live_cpu_unmarked_ns_total",
                     ctr->live_cpu_unmarked_ns_total);
    cjson_add_uint64(root, "live_cpu_gate_fallback_total",
                     ctr->live_cpu_gate_fallback_total);
    /* #128 deferred per-query attribution (query_attr.h): how much live
     * foreground time was attributed to an id reported AFTER the interval
     * closed (a parse-phase lock wait), how much landed in the explicit
     * "unattributed" bucket, and how often a pid's pending list spilled. */
    cjson_add_uint64(root, "live_query_backfilled_ns_total",
                     d->event_accum ? d->event_accum->qattr_backfilled_ns : 0);
    cjson_add_uint64(root, "live_query_unattributed_ns_total",
                     d->event_accum ? d->event_accum->qattr_unattributed_ns : 0);
    cjson_add_uint64(root, "live_query_pending_overflow_total",
                     d->event_accum ? d->event_accum->qattr_pending_overflow : 0);
    /* …and the sampled tier's incoherent pairs: an idle wait event read for
     * a backend the same tick's status read found inside a command. Those
     * samples do NOT close the command (they would strand a parse-phase
     * wait in the unattributed bucket); a nonzero count is the read skew,
     * or client waits inside a command (COPY FROM STDIN). */
    cjson_add_uint64(root, "sampled_idle_in_command_total",
                     ctr->sampled_idle_in_command_total);
    /* …and the summary writer's twin: records that kept their emission-time
     * id because its per-pid table was full, and deferred time no command
     * claimed (the summary has no unattributed bucket). */
    cjson_add_uint64(root, "summary_query_attr_table_full_total",
                     d->summary_writer ? d->summary_writer->qattr_table_full_total : 0);
    cjson_add_uint64(root, "summary_query_unattributed_ns_total",
                     d->summary_writer ? d->summary_writer->qattr_unattributed_ns_total : 0);
    /* Seconds that could not be serialised or compressed: each one is a HOLE
     * in every summaries-path window that covers it. Individual failures also
     * print to stderr, which a running daemon's operator never sees -- so the
     * count belongs here, in the control socket's `metrics` reply (this is
     * build_metrics(); `status` does NOT embed it, and saying "status" sends an
     * operator looking in the wrong place). Must be 0. */
    cjson_add_uint64(root, "summary_flush_failures_total",
                     d->summary_writer ? d->summary_writer->flush_failures_total : 0);
    /* Wait events that could not claim a per-second events[] slot (1024 per
     * second). Nonzero means the per-event breakdown and any class/event
     * FILTERED total are incomplete for those seconds, while unfiltered DB
     * Time and Idle stay exact -- a discrepancy with no other signal.
     * Counts WRITTEN seconds only: a second lost to a failed rotation or close
     * is discarded without being rolled up, so read this together with
     * summary_flush_failures_total rather than as a complete total. */
    cjson_add_uint64(root, "summary_events_overflow_total",
                     d->summary_writer ? d->summary_writer->events_overflow_total : 0);
    /* #277: events that arrived after their second had already been written
     * and were therefore counted in the NEXT second instead. Counts are
     * conserved either way; a persistently nonzero rate means ticks are
     * starved or the event ring is backing up, and the per-second timeline
     * is skewed by up to one second -- which is otherwise indistinguishable
     * from a clean capture. Markers and EXIT records are clamped before
     * accum_event drops them, so they are counted here while contributing
     * no time: treat this as a rate signal, not an exact event count. */
    cjson_add_uint64(root, "summary_late_events_folded_total",
                     d->summary_writer ? d->summary_writer->late_events_folded_total : 0);
    /* v6 (#315/#376): records the summary dropped because their pid is an
     * io_worker, and their ns. The raw paths exclude the same records, so
     * these two are how the agreement is CHECKED in production rather than
     * assumed: compare them against the raw io_worker total for the same
     * window (time_model's cat_ms[io_worker] plus the Activity:IoWorkerMain
     * idle the raw path drops). A shortfall is the known residual --
     * pgwt_live_pid_cat_flag answers "foreground" until a pid's metadata is
     * parsed, so an io_worker's earliest records are accumulated, while the
     * server tags them post hoc from backends.jsonl and excludes them. */
    cjson_add_uint64(root, "summary_io_worker_events_excluded_total",
                     d->summary_writer
                         ? d->summary_writer->io_worker_events_excluded_total : 0);
    cjson_add_uint64(root, "summary_io_worker_ns_excluded_total",
                     d->summary_writer
                         ? d->summary_writer->io_worker_ns_excluded_total : 0);

    /* ── Open-interval crediting (#379 v8, #424 v9) ──────────────────────
     * The summary writer spreads a still-open interval across the seconds it
     * covers instead of charging its closing second. These counters are the
     * only way to tell a PRODUCER that feeds it observations from one that
     * does not: a daemon whose live loop never notes an open on-CPU gap reads
     * credited_ns == 0 with refused_total climbing, and every other number in
     * this response still looks perfectly healthy. Unobservable before these
     * keys existed, which is why they are published even though nothing
     * gates on them. */
    cjson_add_uint64(root, "summary_open_credited_ns_total",
                     d->summary_writer
                         ? d->summary_writer->open_credited_ns_total : 0);
    cjson_add_uint64(root, "summary_open_credit_records_total",
                     d->summary_writer
                         ? d->summary_writer->open_credit_records_total : 0);
    /* The v8 refusal of an OPEN on-CPU gap, counted rather than silent: it is
     * what this daemon does until the v9 writer and the live-loop observation
     * are in the same build. Non-zero here with
     * summary_open_cpu_credited_ns at 0 is the honest signature of that
     * state, and cpu_open_uncredited_ns is how much time it cost. */
    cjson_add_uint64(root, "summary_open_cpu_refused_total",
                     d->summary_writer
                         ? d->summary_writer->open_cpu_refused_total : 0);
    cjson_add_uint64(root, "summary_cpu_open_uncredited_ns_total",
                     d->summary_writer
                         ? d->summary_writer->cpu_open_uncredited_ns_total : 0);
    /* #430: the seal's own file lifecycle, published because the fix is
     * otherwise unobservable in production. reopens_total > 0 says the
     * rotation window really was sealed by the ticks rather than waiting for
     * a record; reopen_failures_total > 0 says some seconds are OWED and the
     * window they belong to is short by that much. A zero pair on a capture
     * long enough to rotate means either no rotation happened or nothing was
     * open across one — not that the pause is gone. */
    cjson_add_uint64(root, "summary_open_seal_reopens_total",
                     d->summary_writer
                         ? d->summary_writer->open_seal_reopens_total : 0);
    cjson_add_uint64(root, "summary_open_seal_reopen_failures_total",
                     d->summary_writer
                         ? d->summary_writer->open_seal_reopen_failures_total
                         : 0);
    /* The FIRST open failing is a different event from a reopen failing --
     * "this capture produced no summary file at all" rather than "recording
     * stopped mid-capture with seconds owed" -- and both reach the push
     * path, which is why they are two keys and not one. */
    cjson_add_uint64(root, "summary_first_open_failures_total",
                     d->summary_writer
                         ? d->summary_writer->first_open_failures_total : 0);
    /* #447: a record can be OLDER than the file it is written into (an
     * event's second began before it arrived; a seal reopen writes a second
     * that completed ~1.1 s earlier), and the reader places a file by its
     * header start and prunes any file starting after the window's end. So
     * the header start is widened to cover the record. widened_total > 0 is
     * normal and expected on any capture that rotates; widen_failures_total
     * > 0 means some file is placed conservatively LATE and a narrow window
     * can miss its earliest records -- the number that says a window may be
     * short rather than leaving it to be assumed. */
    cjson_add_uint64(root, "summary_file_start_widened_total",
                     d->summary_writer
                         ? d->summary_writer->file_start_widened_total : 0);
    cjson_add_uint64(root, "summary_file_start_widen_failures_total",
                     d->summary_writer
                         ? d->summary_writer->file_start_widen_failures_total
                         : 0);
    /* The `#if PGWT_SUMMARY_VERSION >= 9` agent/424-daemon wrapped
     * these in is deliberately NOT taken: the constant is 9, so it
     * is true today and buys nothing, while tests/test_control.sh
     * asserts all seven keys by name unconditionally. A live
     * conditional over a version constant is how the producer got
     * hidden the first time. */
    /* v9 (#424). THE CAPABILITY IS DORMANT: no producer feeds the writer an
     * open-CPU observation, deliberately (see the note at the v8
     * pgwt_summary_note_open call in src/map_reader.c). These counters are
     * published anyway, because they are what MADE that visible — on a
     * gate-2 capture with watchpoints attached and cpu_ns_total moving
     * 0 -> 23.7 s, credited, refused AND uncredited all read zero, which
     * says the writer was never asked rather than that it declined.
     *
     * THE FOURTH OUTCOME, found by reading the writer rather than by
     * reasoning from the counters: a note can be ACCEPTED and then credited
     * by nobody, moving no counter at all.
     *   - crediting happens only in writer_seal_through()
     *     (src/summary_writer.c), which used to begin
     *     `if (!w->fp) return;`;
     *   - its only two callers are pgwt_summary_push_event() and
     *     pgwt_summary_flush_completed();
     *   - w->fp is NULL before the first record opens the file lazily, and
     *     again after every rotation.
     * So on a capture whose in-flight statements are pure CPU — emitting no
     * wait boundaries, hence no pushed records — the open interval is noted,
     * nothing seals, and credited/refused/uncredited all stay 0. That is
     * NOT "the producer never ran", which is what three zeros look like.
     *
     * HALF OF THAT IS FIXED (#430), and the half that is not is the half
     * that matters here. writer_seal_through now REOPENS the file itself
     * after a rotation, so the post-rotation case no longer stalls and
     * summary_open_seal_reopens_total below is non-zero whenever it
     * happened. The PRE-FIRST-RECORD case stands exactly as described: with
     * no file ever opened there is no clock anchor, the seal must invent
     * nothing, and a pure-CPU capture that never pushes a record still reads
     * three zeros. So this paragraph's discriminator is still the right one
     * — only its second cause is gone.
     *
     * The reading that separates them, and this code predicts its value:
     * summary_open_credited_ns_total (the v8 WAIT crediting, above) in the
     * same capture.
     *   zero too  -> nothing sealed at all; the seal trigger is the bug and
     *                re-wiring the producer will not move a single counter;
     *   non-zero  -> seconds were sealed, so the CPU path really was asked
     *                and declined somewhere, and THEN the producer is worth
     *                looking at.
     * Neither is distinguishable without a live capture, which is why the
     * wiring was reverted rather than guessed at.
     *
     * The rest are the writer's own self-checks: eff_rejected_total should
     * read 0 in any build (the producer passes a classification the writer
     * validates), class_divergent_ns counts the time whose majority answer
     * changed after the class was frozen at first credit, tier_mismatch
     * counts a measurement tier that changed mid-interval, and overcredit_ns
     * counts credit the per-second cap had to refuse.
     *
     * TEMPORARY GUARD, not a supported configuration. These fields arrive
     * with the v9 writer, which is on another branch; the conditional exists
     * only so this branch builds and box-checks on its own, and the
     * integration agent DELETES it when v9 folds in. At that moment add these
     * seven keys to tests/test_control.sh's required-key list, by name
     * (they report a dormant capability honestly — zeros that mean "nobody
     * asked", not "nothing happened"):
     *
     *     summary_open_cpu_credited_ns
     *     summary_open_cpu_measured_ns
     *     summary_open_cpu_unknown_ns
     *     summary_open_cpu_eff_rejected_total
     *     summary_open_cpu_class_divergent_ns
     *     summary_open_cpu_tier_mismatch_total
     *     summary_open_cpu_overcredit_ns
     *
     * (that file is a live-tier test shared with other branches, which is why
     * this branch does not edit it). Until then the keys are absent and the
     * daemon reports the v8 refusal in summary_open_cpu_refused_total /
     * summary_cpu_open_uncredited_ns_total above — visible, never silent. */
    cjson_add_uint64(root, "summary_open_cpu_credited_ns",
                     d->summary_writer
                         ? d->summary_writer->open_cpu_credited_ns_total : 0);
    cjson_add_uint64(root, "summary_open_cpu_measured_ns",
                     d->summary_writer
                         ? d->summary_writer->open_cpu_measured_ns_total : 0);
    cjson_add_uint64(root, "summary_open_cpu_unknown_ns",
                     d->summary_writer
                         ? d->summary_writer->open_cpu_unknown_ns_total : 0);
    cjson_add_uint64(root, "summary_open_cpu_eff_rejected_total",
                     d->summary_writer
                         ? d->summary_writer->open_cpu_eff_rejected_total : 0);
    cjson_add_uint64(root, "summary_open_cpu_class_divergent_ns",
                     d->summary_writer
                         ? d->summary_writer->open_cpu_class_divergent_ns_total
                         : 0);
    cjson_add_uint64(root, "summary_open_cpu_tier_mismatch_total",
                     d->summary_writer
                         ? d->summary_writer->open_cpu_tier_mismatch_total : 0);
    cjson_add_uint64(root, "summary_open_cpu_overcredit_ns",
                     d->summary_writer
                         ? d->summary_writer->open_cpu_overcredit_ns_total : 0);

    cjson_add_uint64(root, "io_worker_samples_total",
                     ctr->io_worker_samples_total);
    cjson_add_uint64(root, "io_worker_busy_total",
                     ctr->io_worker_busy_total);
    cjson_add_uint64(root, "noncmd_cpu_samples_total",
                     ctr->noncmd_cpu_samples_total);
    /* #294: the read-order recheck. at_risk is the on-CPU population the CPU
     * gate was about to reject; recovered is how many a fresh, same-predicate
     * status read found inside a command after all; read_failed is how many
     * could not be re-read and therefore stayed dropped — a persistently
     * nonzero read_failed is lost CPU samples, not zero of them.
     * at_risk == recovered + confirmed_closed + read_failed, and the two the
     * recheck did not rescue (confirmed_closed + read_failed) are what
     * noncmd_cpu_samples_total then counts, so read_failed is a SUBSET of
     * noncmd, not a separate term. What remains in noncmd is the genuine
     * between-command on-CPU churn the gate exists to exclude. */
    cjson_add_uint64(root, "cmd_gate_order_at_risk_total",
                     ctr->cmd_gate_order_at_risk_total);
    cjson_add_uint64(root, "cmd_gate_order_recovered_total",
                     ctr->cmd_gate_order_recovered_total);
    cjson_add_uint64(root, "cmd_gate_order_read_failed_total",
                     ctr->cmd_gate_order_read_failed_total);
    cjson_add_uint64(root, "cmd_gate_recovered_total",
                     ctr->cmd_gate_recovered_total);

    /* Provider self-metrics. ringbuf_drops_total is the full tier's BPF-side
     * event_ringbuf drop count (A2 wired this; A0 deliberately omitted it). */
    cjson_add_uint64(root, "ringbuf_drops_total", pm.ringbuf_drops_total);

    /* Trace writer stats (0 when recording is disabled) */
    cjson_add_uint64(root, "trace_events_written_total",
                     d->event_writer ? d->event_writer->total_events_written : 0);
    cjson_add_uint64(root, "trace_bytes_written_total",
                     d->event_writer ? d->event_writer->total_bytes_written : 0);

    /* Escalation accounting (A4). ESC-9: tier is derived the SAME way as
     * status — a fixed EXACT provider (--mode full) reports "escalated", never
     * a bogus "sampled". */
    cJSON_AddStringToObject(root, "tier", daemon_tier_str(d));
    cJSON_AddBoolToObject(root, "escalation_active", d->escalation.active);
    cJSON_AddNumberToObject(root, "escalation_seconds_remaining",
                            pgwt_escalation_remaining_s(d));
    cJSON_AddNumberToObject(root, "escalation_budget_remaining_s",
                            pgwt_escalation_budget_remaining_s(d));
    cJSON_AddBoolToObject(root, "escalation_budget_unlimited",
                          d->escalation.budget_unlimited);
    cjson_add_uint64(root, "escalation_windows_total",
                     d->escalation.windows_total);
    cjson_add_uint64(root, "escalation_denied_total",
                     d->escalation.denied_total);
    cjson_add_uint64(root, "escalation_budget_closed_total",
                     d->escalation.budget_closed_total);

    /* Anomaly-trigger accounting (A5). 0 when not in tiered mode. */
    cjson_add_uint64(root, "anomaly_fires_total", d->anomaly.fires_total);
    cjson_add_uint64(root, "anomaly_cpu_saturation_fires_total",
                     d->anomaly.cpu_saturation_fires_total);
    cjson_add_uint64(root, "anomaly_near_total", d->anomaly.near_total);
    cjson_add_uint64(root, "anomaly_dropped_budget_total",
                     d->anomaly.dropped_budget);
    cjson_add_uint64(root, "anomaly_dropped_cooldown_total",
                     d->anomaly.dropped_cooldown);
    cJSON_AddNumberToObject(root, "anomaly_baseline_aas",
                            d->anomaly.baseline_aas);
    cJSON_AddBoolToObject(root, "anomaly_cpu_cusum_enabled",
                          d->anomaly.enabled
                          && d->anomaly.cpu_cusum_enabled
                          && d->anomaly.aas_factor
                             < PGWT_ANOMALY_DISABLE_AAS_FACTOR);
    cJSON_AddNumberToObject(root, "anomaly_cpu_cusum",
                            d->anomaly.cpu_cusum);

    return root;
}

static cJSON *build_error(const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", 0);
    cJSON_AddStringToObject(root, "error", msg);
    return root;
}

/* {"cmd":"escalate","duration_s":N,"reason":"..."} → grant a bounded,
 * budgeted full-fidelity window. Acks with the granted seconds, or denies
 * with a reason (over budget / not tiered / bad args). An escalate while a
 * window is already open EXTENDS its deadline (subject to budget). */
static cJSON *build_escalate(struct pgwt_daemon *d, cJSON *req)
{
    cJSON *dur = cJSON_GetObjectItem(req, "duration_s");
    int duration_s = cJSON_IsNumber(dur) ? (int)dur->valuedouble : 60;

    cJSON *root = cJSON_CreateObject();
    int granted = 0;
    const char *why = NULL;
    if (pgwt_escalate(d, duration_s, PGWT_ESC_REASON_MANUAL,
                      &granted, &why, NULL) != 0) {
        cJSON_AddBoolToObject(root, "ok", 0);
        cJSON_AddStringToObject(root, "error",
                                why ? why : "escalation denied");
        cJSON_AddNumberToObject(root, "budget_remaining_s",
                                pgwt_escalation_budget_remaining_s(d));
        return root;
    }
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON_AddBoolToObject(root, "escalated", 1);
    cJSON_AddNumberToObject(root, "granted_s", granted);
    cJSON_AddNumberToObject(root, "seconds_remaining",
                            pgwt_escalation_remaining_s(d));
    cJSON_AddNumberToObject(root, "budget_remaining_s",
                            pgwt_escalation_budget_remaining_s(d));
    return root;
}

/* {"cmd":"deescalate"} → detach watchpoints now (idempotent). */
static cJSON *build_deescalate(struct pgwt_daemon *d)
{
    cJSON *root = cJSON_CreateObject();
    if (!d->escalation.enabled) {
        cJSON_AddBoolToObject(root, "ok", 0);
        cJSON_AddStringToObject(root, "error",
                                "escalation requires --mode tiered");
        return root;
    }
    pgwt_deescalate(d, PGWT_ESC_REASON_REQUEST);
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON_AddBoolToObject(root, "escalated", 0);
    cJSON_AddNumberToObject(root, "budget_remaining_s",
                            pgwt_escalation_budget_remaining_s(d));
    return root;
}

/* {"cmd":"open_intervals"} → every live backend's still-OPEN stretch, plus the
 * clock anchor and the publication watermark the consumer needs to reconcile
 * them against the trace (#424). See src/open_snapshot.h for the frozen field
 * shape and why a degraded answer is a STATUS rather than a short array.
 *
 * The snapshot is ~400 KB at MAX_BACKENDS, so it is heap-allocated and sent
 * through the client's output queue; it never touches the 4 KB request
 * buffer. */
static cJSON *build_open_intervals(struct pgwt_daemon *d)
{
    struct pgwt_open_snapshot *s =
        (struct pgwt_open_snapshot *)calloc(1, sizeof(*s));
    if (!s) {
        /* Out of memory is UNAVAILABLE, not an empty success: the caller must
         * label the pane, not silently drop in-flight time. */
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", 0);
        cJSON_AddStringToObject(root, "cmd", PGWT_OPEN_INTERVALS_CMD);
        cJSON_AddStringToObject(root, "status", "unavailable");
        cJSON_AddStringToObject(root, "reason",
                                "daemon out of memory for the snapshot");
        cJSON_AddBoolToObject(root, "complete", 0);
        return root;
    }
    pgwt_collect_open_intervals(d, s);
    cJSON *root = pgwt_open_snapshot_to_json(s, PGWT_OPEN_INTERVALS_REPLY_MAX);
    free(s);
    if (!root)
        root = build_error("open_intervals: response serialisation failed");
    return root;
}

/* ── Client handling ──────────────────────────────────────── */

static void client_drop(struct pgwt_control *c, struct pgwt_control_client *cl)
{
    if (cl->fd < 0)
        return;
    epoll_ctl(c->epoll_fd, EPOLL_CTL_DEL, cl->fd, NULL);
    close(cl->fd);
    cl->fd = -1;
    cl->len = 0;
    cl->want_write = false;
    pgwt_outq_reset(&cl->out);
}

/* Register/deregister EPOLLOUT for a client with pending output. Level-
 * triggered epoll: leaving EPOLLOUT armed on a drained queue would spin the
 * daemon loop, so it is armed only while bytes are actually waiting. */
static int client_arm_write(struct pgwt_control *c,
                            struct pgwt_control_client *cl, bool want)
{
    if (cl->fd < 0 || cl->want_write == want)
        return 0;
    struct epoll_event ev;
    ev.events = EPOLLIN | (want ? EPOLLOUT : 0);
    ev.data.fd = cl->fd;
    if (epoll_ctl(c->epoll_fd, EPOLL_CTL_MOD, cl->fd, &ev) != 0)
        return -1;
    cl->want_write = want;
    return 0;
}

static ssize_t client_write_cb(void *ctx, const char *p, size_t n)
{
    int fd = *(int *)ctx;
    return send(fd, p, n, MSG_NOSIGNAL);
}

/* Push whatever the socket will take. Returns -1 if the client must be
 * dropped. */
static int client_pump(struct pgwt_control *c, struct pgwt_control_client *cl)
{
    if (cl->fd < 0)
        return -1;
    int rc = pgwt_outq_flush(&cl->out, client_write_cb, &cl->fd);
    if (rc < 0)
        return -1;
    return client_arm_write(c, cl, rc == 0);
}

/* Queue one JSON object + '\n' and push as much as the socket takes. Returns
 * 0 on success, -1 on failure (caller drops the client). MSG_NOSIGNAL: a
 * client that disconnects mid-response must not SIGPIPE the daemon.
 *
 * #424: this used to loop on send() and treat EAGAIN as "the client stopped
 * reading", dropping it. That was adequate while every reply fit one socket
 * buffer and is wrong for a reply of hundreds of KB, where a short write is
 * the NORMAL case — the client would see a truncated JSON line, i.e. a
 * partial answer presented as data. The remainder is now queued and finished
 * on EPOLLOUT; only a hard write error or a client that stops reading until
 * the backlog cap is hit gets dropped. */
static int client_send_json(struct pgwt_control *c,
                            struct pgwt_control_client *cl, cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str)
        return -1;

    size_t len = strlen(str);
    str[len] = '\n';  /* overwrite NUL — cJSON allocates len+1 */
    int rc = pgwt_outq_append(&cl->out, str, len + 1);
    free(str);
    if (rc != 0)
        return -1;      /* backlog cap: a correct reply can no longer be sent */
    return client_pump(c, cl);
}

/* Handle one complete request line. Returns 0 on success, -1 if the
 * client should be dropped (send failure). */
static int handle_request(struct pgwt_control *c,
                          struct pgwt_control_client *cl, const char *line)
{
    cJSON *req = cJSON_Parse(line);
    if (!req)
        return client_send_json(c, cl, build_error("invalid json"));

    cJSON *cmd = cJSON_GetObjectItem(req, "cmd");
    cJSON *resp;
    if (!cJSON_IsString(cmd) || !cmd->valuestring)
        resp = build_error("missing cmd");
    else if (strcmp(cmd->valuestring, "status") == 0)
        resp = build_status(c->d);
    else if (strcmp(cmd->valuestring, "metrics") == 0)
        resp = build_metrics(c->d);
    else if (strcmp(cmd->valuestring, "escalate") == 0)
        resp = build_escalate(c->d, req);
    else if (strcmp(cmd->valuestring, "deescalate") == 0)
        resp = build_deescalate(c->d);
    else if (strcmp(cmd->valuestring, PGWT_OPEN_INTERVALS_CMD) == 0)
        resp = build_open_intervals(c->d);
    else
        resp = build_error("unknown command");

    cJSON_Delete(req);
    return client_send_json(c, cl, resp);
}

static void handle_client_readable(struct pgwt_control *c,
                                   struct pgwt_control_client *cl)
{
    for (;;) {
        if (cl->len >= sizeof(cl->buf) - 1) {
            /* No newline within buffer — protocol violation */
            client_send_json(c, cl, build_error("request too long"));
            client_drop(c, cl);
            return;
        }

        ssize_t r = read(cl->fd, cl->buf + cl->len,
                         sizeof(cl->buf) - 1 - cl->len);
        if (r == 0) {
            /* Client disconnected (possibly mid-request) — discard */
            client_drop(c, cl);
            return;
        }
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;  /* drained */
            client_drop(c, cl);
            return;
        }
        cl->len += (size_t)r;

        /* Process complete lines */
        char *nl;
        while ((nl = memchr(cl->buf, '\n', cl->len)) != NULL) {
            *nl = '\0';
            size_t line_len = (size_t)(nl - cl->buf) + 1;

            if (cl->buf[0] != '\0' &&
                handle_request(c, cl, cl->buf) != 0) {
                client_drop(c, cl);
                return;
            }

            memmove(cl->buf, cl->buf + line_len, cl->len - line_len);
            cl->len -= line_len;
        }
    }
}

static void handle_accept(struct pgwt_control *c)
{
    for (;;) {
        int fd = accept4(c->listen_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            return;  /* EAGAIN: done; other errors: nothing to do */
        }

        struct pgwt_control_client *cl = NULL;
        for (int i = 0; i < PGWT_CTRL_MAX_CLIENTS; i++) {
            if (c->clients[i].fd < 0) {
                cl = &c->clients[i];
                break;
            }
        }
        if (!cl) {
            /* All slots busy — refuse politely */
            const char *msg = "{\"ok\":false,\"error\":\"too many clients\"}\n";
            send(fd, msg, strlen(msg), MSG_NOSIGNAL);
            close(fd);
            continue;
        }

        cl->fd = fd;
        cl->len = 0;
        cl->want_write = false;
        pgwt_outq_init(&cl->out);

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        if (epoll_ctl(c->epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
            close(fd);
            cl->fd = -1;
        }
    }
}

/* ── Public API ───────────────────────────────────────────── */

int pgwt_control_init(struct pgwt_control *c, struct pgwt_daemon *d,
                      int epoll_fd)
{
    memset(c, 0, sizeof(*c));
    c->listen_fd = -1;
    c->epoll_fd = epoll_fd;
    c->d = d;
    for (int i = 0; i < PGWT_CTRL_MAX_CLIENTS; i++)
        c->clients[i].fd = -1;

    int n = snprintf(c->sock_path, sizeof(c->sock_path), "%s/pgwt.sock",
                     d->trace_dir);
    struct sockaddr_un addr;
    if (n < 0 || (size_t)n >= sizeof(addr.sun_path)) {
        fprintf(stderr, "WARN: control socket path too long: %s/pgwt.sock\n",
                d->trace_dir);
        return -1;
    }

    /* ESC-10: a plain unlink() lets a second daemon steal a LIVE daemon's
     * control plane (and race for the same trace dir). Liveness-probe first:
     * try to connect to the existing socket. If a daemon answers, refuse
     * startup loudly; only unlink a socket that is stale (connect refused) or a
     * non-socket path. */
    {
        struct stat sb;
        if (lstat(c->sock_path, &sb) == 0) {
            if (S_ISSOCK(sb.st_mode)) {
                int probe = socket(AF_UNIX,
                                   SOCK_STREAM | SOCK_CLOEXEC, 0);
                if (probe >= 0) {
                    struct sockaddr_un pa;
                    memset(&pa, 0, sizeof(pa));
                    pa.sun_family = AF_UNIX;
                    memcpy(pa.sun_path, c->sock_path, (size_t)n + 1);
                    int crc = connect(probe, (struct sockaddr *)&pa,
                                      sizeof(pa));
                    close(probe);
                    if (crc == 0) {
                        fprintf(stderr,
                                "FATAL: another pg_wait_tracer daemon is "
                                "already running on %s\n"
                                "  Refusing to steal its control socket. Stop "
                                "it first, or use a different --trace-dir.\n",
                                c->sock_path);
                        return -2;   /* live-daemon conflict: fatal (ESC-10) */
                    }
                }
            }
            /* Stale socket (or non-socket file) from a crashed daemon. */
            unlink(c->sock_path);
        }
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "WARN: control socket: socket(): %s\n",
                strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, c->sock_path, (size_t)n + 1);

    /* Mode 0600 from the moment of creation (no chmod window) */
    mode_t old_umask = umask(0177);
    int bind_rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old_umask);
    if (bind_rc != 0) {
        fprintf(stderr, "WARN: control socket: bind(%s): %s\n",
                c->sock_path, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 8) != 0) {
        fprintf(stderr, "WARN: control socket: listen(%s): %s\n",
                c->sock_path, strerror(errno));
        close(fd);
        unlink(c->sock_path);
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        fprintf(stderr, "WARN: control socket: epoll_ctl: %s\n",
                strerror(errno));
        close(fd);
        unlink(c->sock_path);
        return -1;
    }

    c->listen_fd = fd;
    return 0;
}

int pgwt_control_handle_fd(struct pgwt_control *c, int fd)
{
    if (fd == c->listen_fd) {
        handle_accept(c);
        return 1;
    }
    for (int i = 0; i < PGWT_CTRL_MAX_CLIENTS; i++) {
        if (c->clients[i].fd == fd) {
            struct pgwt_control_client *cl = &c->clients[i];
            /* The caller does not say WHICH event fired, so finish any
             * pending write first (#424) and then try to read: a
             * writable-only wakeup just finds EAGAIN on the read and
             * returns. */
            if (pgwt_outq_pending(&cl->out) > 0 && client_pump(c, cl) != 0) {
                client_drop(c, cl);
                return 1;
            }
            handle_client_readable(c, cl);
            return 1;
        }
    }
    return 0;
}

void pgwt_control_cleanup(struct pgwt_control *c)
{
    for (int i = 0; i < PGWT_CTRL_MAX_CLIENTS; i++)
        client_drop(c, &c->clients[i]);
    if (c->listen_fd >= 0) {
        epoll_ctl(c->epoll_fd, EPOLL_CTL_DEL, c->listen_fd, NULL);
        close(c->listen_fd);
        c->listen_fd = -1;
        unlink(c->sock_path);
    }
}
