/* daemon.c — Main event loop: epoll on ring buffer, timer, signals */
#include "daemon.h"
#include "backend.h"
#include "control.h"
#include "discovery.h"
#include "event_stream.h"
#include "event_writer.h"
#include "map_reader.h"
#include "output.h"
#include "perf_event.h"
#include "provider_coop.h"
#include "wait_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/resource.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "pg_wait_tracer.skel.h"

#define PGWT_DEBUG_BLOCK_NS (50ULL * 1000ULL * 1000ULL)
#define PGWT_TEST_EVENT_DELAY_COUNT 1024U
#define PGWT_EVENT_DRAIN_CALLBACK_BUDGET 64U

uint64_t pgwt_debug_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void pgwt_debug_block_report(const char *op, pid_t pid, uint64_t started_ns)
{
    uint64_t ended_ns = pgwt_debug_monotonic_ns();
    if (ended_ns >= started_ns && ended_ns - started_ns > PGWT_DEBUG_BLOCK_NS)
        fprintf(stderr, "STATEDUMP-BLOCK: op=%s pid=%d dur_ms=%.3f\n",
                op, pid, (double)(ended_ns - started_ns) / 1e6);
}

/* Attribute a slow event-ring consume to callback work versus libbpf's own
 * drain loop. ring_buffer__consume() can repeatedly reload producer_pos and
 * therefore has no finite work bound while a producer keeps refilling the
 * ring. The callback counters make that case decisive: many individually
 * short callbacks whose aggregate time fills the whole block are an
 * unbounded drain, while one slow callback stage is reported by the finer
 * probes in event_stream.c. */
static bool consume_event_ring(struct pgwt_daemon *d, const char *op,
                               bool bounded)
{
    uint64_t started_ns = d->debug_dump_state
                        ? pgwt_debug_monotonic_ns() : 0;
    uint64_t callbacks_before = d->debug_event_callbacks_total;
    uint64_t callback_ns_before = d->debug_event_callback_ns_total;

    d->event_drain_callbacks_current = 0;
    d->event_drain_callback_limit =
        bounded && !d->test_no_event_drain_budget
        ? PGWT_EVENT_DRAIN_CALLBACK_BUDGET : 0;
    int rc = ring_buffer__consume(d->event_rb);
    bool yielded = d->event_drain_callback_limit != 0
                && d->event_drain_callbacks_current
                   >= d->event_drain_callback_limit
                && rc < 0;
    d->event_drain_callback_limit = 0;
    if (yielded && d->debug_dump_state)
        d->debug_event_drain_yields++;

    if (!d->debug_dump_state)
        return yielded;

    uint64_t ended_ns = pgwt_debug_monotonic_ns();

    if (ended_ns >= started_ns && ended_ns - started_ns > PGWT_DEBUG_BLOCK_NS) {
        uint64_t callback_ns = d->debug_event_callback_ns_total
                             - callback_ns_before;
        uint64_t total_ns = ended_ns - started_ns;
        uint64_t outside_ns = callback_ns < total_ns
                            ? total_ns - callback_ns : 0;
        fprintf(stderr,
                "STATEDUMP-BLOCK: op=%s pid=0 dur_ms=%.3f callbacks=%llu "
                "callback_ms=%.3f outside_callback_ms=%.3f "
                "max_callback_ms=%.3f rc=%d\n",
                op, (double)total_ns / 1e6,
                (unsigned long long)(d->debug_event_callbacks_total
                                     - callbacks_before),
                (double)callback_ns / 1e6, (double)outside_ns / 1e6,
                (double)d->debug_event_callback_max_ns / 1e6, rc);
    }
    return yielded;
}

/* Ring buffer callback for lifecycle events */
static int handle_lifecycle_event(void *ctx, void *data, size_t data_sz)
{
    struct pgwt_daemon *d = ctx;
    struct pgwt_lifecycle_event *ev = data;

    (void)data_sz;

    d->counters.lifecycle_events_total++;

    const char *op = "lifecycle_unknown";
    uint64_t started_ns = pgwt_debug_block_begin(d);
    switch (ev->type) {
    case PGWT_LIFECYCLE_FORK:
        op = "lifecycle_fork";
        pgwt_handle_fork(d, ev->pid);
        break;
    case PGWT_LIFECYCLE_INIT:
        op = "lifecycle_init";
        pgwt_handle_init(d, ev->pid, ev->addr);
        break;
    case PGWT_LIFECYCLE_EXIT:
        op = "lifecycle_exit";
        pgwt_handle_exit(d, ev->pid);
        break;
    case PGWT_LIFECYCLE_QUERY_TEXT: {
        op = "lifecycle_query_text";
        /* Query text captured by BPF from debug_query_string */
        struct pgwt_query_text_event *qte = data;
        if (d->query_text_capture && qte->query_id != 0) {
            pgwt_qt_store(d->query_text_capture, qte->query_id,
                          qte->text, qte->pid);
        }
        break;
    }
    }
    pgwt_debug_block_end(d, op, ev->pid, started_ns);
    return 0;
}

static void handle_timer(struct pgwt_daemon *d)
{
    if (d->debug_dump_state)
        d->debug_timer_entries++;

    uint64_t expirations;
    uint64_t started_ns = pgwt_debug_block_begin(d);
    ssize_t r = read(d->timer_fd, &expirations, sizeof(expirations));
    pgwt_debug_block_end(d, "timerfd_read", 0, started_ns);
    if (d->debug_dump_state && r == (ssize_t)sizeof(expirations)) {
        d->debug_timer_expirations += expirations;
        if (expirations > d->debug_max_timer_expirations)
            d->debug_max_timer_expirations = expirations;
    }

    /* Health check: is PostgreSQL still alive? */
    if (d->daemon_mode) {
        started_ns = pgwt_debug_block_begin(d);
        int pg_alive_rc = kill(d->postmaster_pid, 0);
        pgwt_debug_block_end(d, "postmaster_health_check",
                             d->postmaster_pid, started_ns);
        if (pg_alive_rc != 0) {
            fprintf(stderr, "\npg_wait_tracer: PostgreSQL (PID %d) stopped\n",
                    d->postmaster_pid);
            d->exit_reason = PGWT_EXIT_PG_DEAD;
            d->running = false;
            return;
        }
    }

    /* Stage 3 refreshes capacity at sampler cadence when the anomaly engine
     * is armed, so the CUSUM consumes every material-change signal. Other
     * modes retain Stage 2's display-cadence refresh for observability. */
    if (!d->anomaly.enabled)
        d->effective_cores = pgwt_effective_cores_refresh(
            &d->cpu_capacity_resolver, d->postmaster_pid);

    /* Print at the first non-PG-death point in the handler. In particular,
     * this is before recovery/sweep and every live-map read: a TIMER line with
     * no later SCAN line means the handler entered and stalled in that prefix;
     * no TIMER line means dispatch never reached the handler. */
    bool mode_uses_wp = pgwt_mode_uses_watchpoints(d);
    if (d->debug_dump_state)
        fprintf(stderr, "STATEDUMP-TIMER: tick=%d mode_uses_wp=%d "
                "lightweight=%d\n",
                d->tick, (int)mode_uses_wp, (int)d->lightweight_mode);

    /* Straddle-race recovery: re-attach any pre-existing backend the one-shot
     * startup scan missed or left in bootstrap limbo (a transient resolve race,
     * fatal for a waitless pure-CPU command whose bootstrap watchpoint never
     * fires). Level-triggered and idempotent — runs before the state_map read
     * so a recovery this tick is reflected in this tick's view. Full/tiered
     * only (no-op in sampled/lightweight, which resolve lazily). */
    if (!d->lightweight_mode && !getenv("PGWT_TEST_NO_RECOVERY")) {
        started_ns = pgwt_debug_block_begin(d);
        pgwt_recover_unattached_backends(d);
        pgwt_debug_block_end(d, "backend_recovery_scan", 0, started_ns);
    }

    /* Stale-seed sweep — the ATTACHED-backend sibling of the recovery above
     * (the seed→arm race, docs/ROADMAP_AND_STATUS.md): reseed any state_map
     * entry frozen on a wait the backend provably left long ago, so the live
     * view stops attributing the open interval (and its CPU) to a dead wait.
     * Deliberately guarded by its OWN test env, not PGWT_TEST_NO_RECOVERY:
     * that guard keeps meaning exactly "no unattached-backend recovery" (the
     * PR #52 negative is untouched — an unattached backend has wp_fd < 0, so
     * this sweep never touches it anyway), and the stale-seed negative test
     * disables ONLY the sweep while the recovery stays production-active,
     * proving the sweep — not the recovery — is the repairing agent. Not run
     * in the startup settle: the seed is by definition fresher than the 1s
     * frozen gate there, so the first timer tick is the earliest a stale
     * entry is distinguishable from a fresh one. */
    if (!d->lightweight_mode && !getenv("PGWT_TEST_NO_STALE_SWEEP")) {
        started_ns = pgwt_debug_block_begin(d);
        pgwt_sweep_stale_state(d);
        pgwt_debug_block_end(d, "stale_state_sweep", 0, started_ns);
    }

    /* ESC-1: enforce the escalation budget mid-window (cheap no-op unless a
     * budget-limited window is open and has reached its cap). */
    started_ns = pgwt_debug_block_begin(d);
    pgwt_escalation_check_budget(d);
    pgwt_debug_block_end(d, "escalation_budget_check", 0, started_ns);

    /* In lightweight mode, read BPF accum_map into event_accum first */
    if (d->lightweight_mode) {
        started_ns = pgwt_debug_block_begin(d);
        pgwt_read_accum_map(d);
        pgwt_debug_block_end(d, "accum_map_read", 0, started_ns);
    }

    /* Copy cumulative event_accum to display accum,
     * then add open intervals from state_map.
     * Sampled mode has no watchpoint-maintained state_map intervals (entries
     * exist only to carry query_id), so reading it would manufacture bogus
     * open intervals — skip it. The live display for sampled mode is not
     * fidelity-aware until A3; recorded SAMPLES blocks carry the real data. */
    struct pgwt_time_model saved_tm = d->accum.tm;
    started_ns = pgwt_debug_block_begin(d);
    pgwt_accum_copy_used(&d->accum, d->event_accum);
    pgwt_debug_block_end(d, "accumulator_copy", 0, started_ns);
    d->accum.prev_tm = saved_tm;
    if (mode_uses_wp) {
        started_ns = pgwt_debug_block_begin(d);
        pgwt_read_state_map(d);
        pgwt_debug_block_end(d, "state_map_read", 0, started_ns);
    }

    if (d->ring.slots) {
        started_ns = pgwt_debug_block_begin(d);
        pgwt_ring_push(&d->ring, &d->accum);
        pgwt_debug_block_end(d, "snapshot_ring_push", 0, started_ns);
    }

    if (!d->quiet) {
        started_ns = pgwt_debug_block_begin(d);
        switch (d->view) {
        case PGWT_VIEW_TIME_MODEL:
            pgwt_print_time_model(d);
            break;
        case PGWT_VIEW_SYSTEM_EVENT:
            pgwt_print_system_event(d);
            break;
        case PGWT_VIEW_SESSION_EVENT:
            pgwt_print_session_event(d);
            break;
        case PGWT_VIEW_HISTOGRAM:
            pgwt_print_histogram(d);
            break;
        case PGWT_VIEW_QUERY_EVENT:
            pgwt_print_query_event(d);
            break;
        case PGWT_VIEW_ACTIVE:
            pgwt_print_active(d);
            break;
        }
        fflush(stdout);
        pgwt_debug_block_end(d, "view_output", 0, started_ns);
    }

    /* GC: sweep backend table every 60s for dead processes.
     * Handles PIDs that exited without triggering on_exit tracepoint
     * (e.g., SIGKILL, or race condition during high churn). */
    if (d->tick > 0 && d->tick % 60 == 0) {
        started_ns = pgwt_debug_block_begin(d);
        for (int i = 0; i < d->backends.count; i++) {
            struct pgwt_backend *be = &d->backends.entries[i];
            if (be->is_alive && be->pid > 0 && kill(be->pid, 0) != 0) {
                if (d->verbose)
                    fprintf(stderr, "INFO: GC: PID %d no longer alive, cleaning up\n",
                            be->pid);
                pgwt_handle_exit(d, be->pid);
            }
        }
        pgwt_debug_block_end(d, "backend_gc", 0, started_ns);
    }

    /* Trace file: check hourly rotation, periodic cleanup */
    if (d->event_writer) {
        started_ns = pgwt_debug_block_begin(d);
        pgwt_writer_check_rotation(d->event_writer);
        if (d->tick > 0 && d->tick % 60 == 0)
            pgwt_writer_cleanup_old_files(d->event_writer);
        pgwt_debug_block_end(d, "event_writer_maintenance", 0, started_ns);
    }
    if (d->summary_writer) {
        started_ns = pgwt_debug_block_begin(d);
        pgwt_summary_flush(d->summary_writer);
        pgwt_summary_check_rotation(d->summary_writer);
        if (d->tick > 0 && d->tick % 60 == 0)
            pgwt_summary_cleanup_old_files(d->summary_writer);
        pgwt_debug_block_end(d, "summary_writer_maintenance", 0, started_ns);
    }

    /* CAP-6: the BPF seen_query_ids dedup map (4096 entries) never ages;
     * once full, query TEXT for new query_ids is silently never captured
     * again. Log it once; seen_query_ids_full_total keeps counting. */
    started_ns = pgwt_debug_block_begin(d);
    uint64_t seen_qids_failures = d->seen_qids_full_logged ? 0
        : pgwt_read_bpf_fail_counter(d, PGWT_BPF_FAIL_SEEN_QIDS);
    pgwt_debug_block_end(d, "bpf_fail_counter_read", 0, started_ns);
    if (!d->seen_qids_full_logged && seen_qids_failures > 0) {
        d->seen_qids_full_logged = true;
        fprintf(stderr,
                "WARN: BPF seen_query_ids map is FULL (4096 unique query_ids)"
                " — query TEXT for new query_ids will no longer be captured "
                "(ids and waits are unaffected). Restart the daemon to reset;"
                " seen_query_ids_full_total counts the misses.\n");
    }

    /* Refresh recent event/sample rates for control-socket metrics */
    if (d->interval > 0) {
        d->counters.events_per_sec =
            (double)(d->counters.events_total - d->counters.prev_events_total)
            / d->interval;
        d->counters.prev_events_total = d->counters.events_total;

        d->counters.samples_per_sec =
            (double)(d->counters.samples_total - d->counters.prev_samples_total)
            / d->interval;
        d->counters.prev_samples_total = d->counters.samples_total;

        /* T2: io_worker utilization over the last display interval. */
        uint64_t iw_s = d->counters.io_worker_samples_total
                      - d->counters.prev_io_worker_samples;
        uint64_t iw_b = d->counters.io_worker_busy_total
                      - d->counters.prev_io_worker_busy;
        d->counters.io_worker_busy_pct =
            iw_s > 0 ? 100.0 * (double)iw_b / (double)iw_s : 0.0;
        d->counters.prev_io_worker_samples = d->counters.io_worker_samples_total;
        d->counters.prev_io_worker_busy    = d->counters.io_worker_busy_total;
    }

    d->tick++;
    if (d->count > 0 && d->tick >= d->count)
        d->running = false;
}

/* SMP-5: arm the sampler timer ONE-SHOT with a +/-10% jittered period.
 * Fixed-phase sampling aliases with periodic workloads (a job that wakes
 * every 100 ms could hide from — or monopolize — a 10 Hz sampler forever);
 * jittering each tick's phase breaks the lock-step. Total sample weight is
 * unaffected: every tick is weighted by the MEASURED inter-tick elapsed
 * time (SMP-3, pgwt_sampler_effective_period). */
static void arm_sample_timer_jittered(struct pgwt_daemon *d)
{
    int hz = d->sample_rate_hz > 0 ? d->sample_rate_hz : 10;
    uint64_t period_ns = 1000000000ULL / (uint64_t)hz;
    uint64_t jitter = period_ns / 10;
    /* period in [0.9p, 1.1p] — rand() is plenty for phase decorrelation. */
    uint64_t armed = period_ns - jitter
                   + (uint64_t)rand() % (2 * jitter + 1);
    struct itimerspec sits = {
        .it_value = { .tv_sec  = (time_t)(armed / 1000000000ULL),
                      .tv_nsec = (long)(armed % 1000000000ULL) },
    };
    timerfd_settime(d->sample_timer_fd, 0, &sits, NULL);
}

static void handle_sample_timer(struct pgwt_daemon *d)
{
    uint64_t expirations = 0;
    uint64_t started_ns = pgwt_debug_block_begin(d);
    ssize_t r = read(d->sample_timer_fd, &expirations, sizeof(expirations));
    pgwt_debug_block_end(d, "sample_timerfd_read", 0, started_ns);
    /* SMP-3: expirations > 1 means the daemon stalled and ticks coalesced —
     * those samples are gone. The sampler compensates by weighting each tick
     * with the MEASURED inter-tick elapsed time (see pgwt_sampler_poll), so
     * the loss is in resolution, not in total weight. Count it so a chronic
     * stall is visible on the control socket. (One-shot arming makes >1
     * unlikely, but a stalled daemon can still miss re-arms late.) */
    if (r == (ssize_t)sizeof(expirations) && expirations > 1)
        d->counters.sampler_ticks_missed_total += expirations - 1;
    /* Re-arm FIRST so a slow poll delays the next tick (measured-elapsed
     * weighting absorbs it) instead of bursting. */
    started_ns = pgwt_debug_block_begin(d);
    arm_sample_timer_jittered(d);
    pgwt_debug_block_end(d, "sample_timer_rearm", 0, started_ns);
    if (d->provider && d->provider->poll) {
        started_ns = pgwt_debug_block_begin(d);
        d->provider->poll(d);
        pgwt_debug_block_end(d, "capture_provider_poll", 0, started_ns);
    }
}

/* CAP-12: raise RLIMIT_NOFILE to what MAX_BACKENDS needs. Full/tiered mode
 * holds up to two perf fds per backend (watchpoint + bootstrap) plus BPF
 * maps/ringbufs, trace files and the control socket; the common default of
 * 1024 is below that, and hitting it turns every further attach into a
 * quiet EMFILE failure (now also loudly reported in perf_event.c). */
static void bump_rlimit_nofile(struct pgwt_daemon *d)
{
    const rlim_t need = MAX_BACKENDS * 2 + 512;
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return;
    if (rl.rlim_cur >= need)
        return;

    struct rlimit want = rl;
    want.rlim_cur = need;
    if (want.rlim_max != RLIM_INFINITY && want.rlim_max < need)
        want.rlim_max = need;   /* root may raise the hard limit */
    if (setrlimit(RLIMIT_NOFILE, &want) == 0) {
        if (d->verbose)
            fprintf(stderr, "INFO: RLIMIT_NOFILE raised %llu -> %llu\n",
                    (unsigned long long)rl.rlim_cur,
                    (unsigned long long)need);
        return;
    }
    /* Could not raise the hard limit (not root?) — take what we can get. */
    want.rlim_max = rl.rlim_max;
    want.rlim_cur = (rl.rlim_max == RLIM_INFINITY || rl.rlim_max > need)
                  ? need : rl.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &want) == 0 && want.rlim_cur >= need)
        return;
    fprintf(stderr,
            "WARN: RLIMIT_NOFILE is %llu (< %llu needed for %d backends). "
            "Watchpoint attach will fail with EMFILE beyond the limit — "
            "raise the hard limit (ulimit -Hn / LimitNOFILE=).\n",
            (unsigned long long)want.rlim_cur, (unsigned long long)need,
            MAX_BACKENDS);
}

static void handle_signal(struct pgwt_daemon *d)
{
    struct signalfd_siginfo si;
    ssize_t r = read(d->signal_fd, &si, sizeof(si));
    (void)r;
    d->running = false;
}

int pgwt_daemon_init(struct pgwt_daemon *d)
{
    int err;

    /* Cache the debug env once per daemon lifecycle. All liveness clocks and
     * duration probes key off this bool; when unset they do no syscalls and
     * emit nothing. Reset the per-run counters for daemon-mode re-attach. */
    d->debug_dump_state = getenv("PGWT_DEBUG_DUMP_STATE") != NULL;
    d->debug_loop_iterations = 0;
    d->debug_timer_entries = 0;
    d->debug_timer_expirations = 0;
    d->debug_max_timer_expirations = 0;
    d->debug_last_loop_ts_ns = 0;
    d->debug_max_loop_gap_ns = 0;
    d->debug_event_callbacks_total = 0;
    d->debug_event_callback_ns_total = 0;
    d->debug_event_callback_max_ns = 0;
    d->debug_event_drain_yields = 0;
    d->debug_timer_settime_rc = 0;
    d->debug_timer_epoll_rc = 0;
    d->test_event_callback_delay_us = 0;
    d->test_event_callback_delays_left = 0;
    d->test_no_event_drain_budget =
        getenv("PGWT_TEST_NO_EVENT_DRAIN_BUDGET") != NULL;
    d->event_drain_callback_limit = 0;
    d->event_drain_callbacks_current = 0;
    if (d->test_no_event_drain_budget)
        fprintf(stderr,
                "WARN: PGWT_TEST_NO_EVENT_DRAIN_BUDGET — event-ring consume "
                "is unbounded (deterministic mode-4 negative; TEST ONLY)\n");

    /* TEST HOOK: slow each of a bounded number of trace-event callbacks so a
     * sustained wait-transition producer deterministically keeps libbpf's
     * drain-until-empty loop non-empty. This reproduces the mode-4 main-loop
     * starvation without relying on 2-vCPU scheduler luck. */
    {
        const char *delay_env = getenv("PGWT_TEST_EVENT_CALLBACK_DELAY_US");
        if (delay_env) {
            char *end = NULL;
            unsigned long delay_us = strtoul(delay_env, &end, 10);
            if (end != delay_env && *end == '\0' && delay_us > 0
                && delay_us <= 100000UL) {
                d->test_event_callback_delay_us = (uint32_t)delay_us;
                d->test_event_callback_delays_left =
                    PGWT_TEST_EVENT_DELAY_COUNT;
                fprintf(stderr,
                        "WARN: PGWT_TEST_EVENT_CALLBACK_DELAY_US=%u — delaying "
                        "the first %u trace callbacks (deterministic "
                        "event-ring drain stall; TEST ONLY)\n",
                        d->test_event_callback_delay_us,
                        PGWT_TEST_EVENT_DELAY_COUNT);
            } else {
                fprintf(stderr,
                        "WARN: ignoring invalid "
                        "PGWT_TEST_EVENT_CALLBACK_DELAY_US=%s "
                        "(expected integer 1..100000)\n",
                        delay_env);
            }
        }
    }

    /* Resolve before the control socket becomes reachable, then refresh from
     * the display timer. A failed read is represented as UNKNOWN/-1; startup
     * remains safe and never substitutes a guessed small capacity. */
    struct pgwt_effective_cores_options capacity_options = {
        .filesystem_root = "/",
        .override_cores = d->anomaly_cpu_capacity,
    };
    pgwt_effective_cores_resolver_init(&d->cpu_capacity_resolver,
                                       &capacity_options);
    d->effective_cores = pgwt_effective_cores_refresh(
        &d->cpu_capacity_resolver, d->postmaster_pid);
    if (d->verbose) {
        if (d->effective_cores.source == PGWT_EFFECTIVE_CORES_SOURCE_UNKNOWN)
            fprintf(stderr, "WARN: target effective CPU capacity unknown\n");
        else
            fprintf(stderr, "INFO: target effective CPU capacity: %.6g "
                    "logical cores (source=%s)\n", d->effective_cores.cores,
                    pgwt_effective_cores_source_name(
                        d->effective_cores.source));
    }

    /* CAP-12: enough fds for a full-size backend registry, before anything
     * starts opening watchpoints. */
    bump_rlimit_nofile(d);

    /* Select the capture provider for this mode. FULL (default) is the
     * original watchpoint path; SAMPLED is the userspace tier; TIERED runs
     * the sampler always-on (escalation engine lands in A4). */
    switch (d->mode) {
    case PGWT_MODE_SAMPLED:
    case PGWT_MODE_TIERED:
        d->provider = &pgwt_provider_sampled;
        break;
    case PGWT_MODE_COOP:
        /* A6 interface freeze: the coop provider is recognized but its
         * start() cleanly reports "not available in this build" (the
         * cooperative tier ships in the separate extension track). */
        d->provider = &pgwt_provider_coop;
        break;
    case PGWT_MODE_FULL:
    default:
        d->provider = &pgwt_provider_full;
        break;
    }

    /* Init backend table and accumulator */
    pgwt_backend_init(&d->backends);
    pgwt_accum_init(&d->accum);

    /* Init ring buffer for windowed analysis */
    if (d->num_windows > 0) {
        int cap = d->windows[d->num_windows - 1] / d->interval + 1;
        if (pgwt_ring_init(&d->ring, cap) != 0) {
            fprintf(stderr, "FATAL: cannot allocate snapshot ring buffer (%d slots)\n", cap);
            return -1;
        }
        if (d->verbose)
            fprintf(stderr, "INFO: ring buffer: %d slots (%.1f MB)\n",
                    cap, (double)cap * sizeof(struct pgwt_snapshot) / 1e6);
    }

    /* Open and configure BPF skeleton */
    d->skel = pg_wait_tracer_bpf__open();
    if (!d->skel) {
        fprintf(stderr, "FATAL: failed to open BPF skeleton\n");
        return -1;
    }

    /* Set rodata constants before loading */
    d->skel->rodata->target_postmaster_pid = d->postmaster_pid;
    d->skel->rodata->my_wait_ptr_addr = d->my_wait_ptr_addr;
    /* PG<17: my_wait_ptr_addr is MyProc (PGPROC*); on_bootstrap adds this
     * offset to *MyProc to reach wait_event_info. 0 on PG17+. */
    d->skel->rodata->pgproc_wait_offset =
        d->use_myproc ? (uint32_t)d->pgproc_wait_offset : 0;
    d->skel->rodata->my_be_entry_addr = d->my_be_entry_addr;
    d->skel->rodata->st_query_id_offset = d->st_query_id_offset;
    d->skel->rodata->lightweight_mode = d->lightweight_mode;
    d->skel->rodata->skip_query_id = d->skip_query_id;

    /* Command-open gate (T2): BackendState enum values for this PG version.
     * PG13-17: STATE_RUNNING=2, STATE_FASTPATH=4. PG18 inserted
     * STATE_STARTING after STATE_UNDEFINED, shifting them to 3/5 (verified
     * against REL_13/16/17/18_STABLE backend_status.h/pgstat.h). An unknown
     * version leaves 0 = gate disabled (loud warning below): client on-CPU
     * samples are then NOT recorded — an honest under-count, never the
     * ungated ~3x over-count. */
    if (d->pg_major_version >= 18) {
        d->skel->rodata->bs_state_running  = 3;
        d->skel->rodata->bs_state_fastpath = 5;
    } else if (d->pg_major_version >= 13) {
        d->skel->rodata->bs_state_running  = 2;
        d->skel->rodata->bs_state_fastpath = 4;
    }

    /* PG13 query attribution (Route B1): the ExecutorStart uprobe walks
     * QueryDesc->plannedstmt->queryId into the state_map. Enabled only when
     * pg_stat_statements is loaded (use_pg13_query_attr). */
    d->skel->rodata->pg13_query_attr = d->use_pg13_query_attr ? 1 : 0;
    d->skel->rodata->pg13_qd_plannedstmt_off = (uint32_t)d->pg13_qd_plannedstmt_off;
    d->skel->rodata->pg13_ps_queryid_off = (uint32_t)d->pg13_ps_queryid_off;
    d->skel->rodata->pg13_qd_sourcetext_off = (uint32_t)d->pg13_qd_sourcetext_off;

    /* CAP-1: state_map is compiled at MAX_BACKENDS entries (the registry
     * capacity). PGWT_STATE_MAP_ENTRIES shrinks it before load — a TEST hook
     * so CI can prove the map-full loud path with a handful of connections
     * instead of >1024 real ones. Never set it in production. */
    {
        const char *sme = getenv("PGWT_STATE_MAP_ENTRIES");
        if (sme && atoi(sme) > 0) {
            uint32_t entries = (uint32_t)atoi(sme);
            bpf_map__set_max_entries(d->skel->maps.state_map, entries);
            fprintf(stderr, "WARN: PGWT_STATE_MAP_ENTRIES=%u — state_map "
                    "shrunk below MAX_BACKENDS (test hook; backends beyond "
                    "this record nothing, loudly)\n", entries);
        }
    }

    /* Resolve debug_query_string to its RUNTIME virtual address.
     *
     * PIE fix (#24 class, T8 §4): the raw ELF st_value from
     * pgwt_find_symbol_offset() is only the runtime VA on non-PIE (ET_EXEC)
     * binaries. On PIE (ET_DYN) builds — every Ubuntu/Debian PGDG package and
     * EL9 — the runtime VA is load_base + st_value, and load_base is the
     * per-process ASLR slide. Both consumers of this address dereference it in
     * a backend's address space: the BPF query-TEXT path via
     * bpf_probe_read_user() (in-task, target runtime VA) and the userspace
     * command-open gate via pread(mem_fd) / process_vm_readv (T2, sampler.c +
     * preseed). The raw vaddr therefore read garbage on PIE, collapsing query
     * text capture AND the straddle CPU gate. Resolve like every other global
     * (MyProc/MyBEEntry): the postmaster shares its load base with every forked
     * backend, so one resolution against postmaster_pid yields the VA valid in
     * all of them. */
    if (d->pg_binary_saved) {
        uint64_t dqs_addr = pgwt_resolve_symbol(d->pg_binary_saved,
                                                 "debug_query_string",
                                                 d->postmaster_pid);
        d->skel->rodata->debug_query_string_addr = dqs_addr;
        /* Userspace copy: the sampler reads this global per-pid each tick as
         * the race-free command-open gate (T2 fix, see sampler.c). */
        d->debug_query_string_addr = dqs_addr;
        if (d->verbose && dqs_addr)
            fprintf(stderr, "INFO: debug_query_string at 0x%lx (runtime VA)\n",
                    (unsigned long)dqs_addr);
    }

    /* T8 measured-CPU capability probe (docs/ROADMAP_AND_STATUS.md).
     * The exact tier already requires kernel BTF (the full-tier gate), so the
     * BPF read of task_struct->se.sum_exec_runtime is available; the userspace
     * seed/flush/live paths read the SAME accumulator via /proc/<pid>/schedstat
     * field 1, which exists whenever CONFIG_SCHED_INFO is set (universal on
     * RHEL and Ubuntu kernels). Probe it once on the daemon's own pid (it has
     * accrued CPU by now, so a working schedstat returns > 0). When present:
     * full measured mode. When absent: legacy gap-inference, reported LOUDLY —
     * never silent. (The S1b perf_event PERF_COUNT_SW_TASK_CLOCK fallback for
     * schedstat-less kernels is deferred; see the PR notes.) */
    d->cpu_accounting = (access("/sys/kernel/btf/vmlinux", R_OK) == 0);
    d->skel->rodata->cpu_accounting = d->cpu_accounting;
    /* TEST HOOK: see pg_wait_tracer.bpf.c test_no_sched_oncpu. Forces the
     * live CPU*=0 straddle repro deterministically by suppressing the
     * sched_switch on_cpu_ts open. Test-only; loud so it can never be missed. */
    if (getenv("PGWT_TEST_NO_SCHED_ONCPU")) {
        d->skel->rodata->test_no_sched_oncpu = true;
        fprintf(stderr, "WARN: PGWT_TEST_NO_SCHED_ONCPU — sched_switch on_cpu_ts "
                "open suppressed (deterministic straddle-CPU repro; TEST ONLY)\n");
    }
    /* DEBUG HOOK (PGWT_DEBUG_DUMP_STATE): see pgwt_debug_dump_state_map — the
     * final state_map is dumped to stderr at shutdown for the reopened
     * straddle live-CPU investigation. Announced here so the hook can never
     * be active silently, same discipline as the PGWT_TEST_* hooks above. */
    if (d->debug_dump_state) {
        fprintf(stderr, "WARN: PGWT_DEBUG_DUMP_STATE — state, timer, and "
                "main-loop liveness will be dumped to stderr (STATEDUMP lines; "
                "DEBUG ONLY, never set in production)\n");
    }
    if (!d->cpu_accounting) {
        /* No BTF → tp_btf/sched_switch can't load: don't try, and fall back
         * to gap-inference (cpu_ns UNKNOWN, no Off-CPU*). */
        bpf_program__set_autoload(d->skel->progs.on_sched_switch, false);
        fprintf(stderr, "WARN: CPU accounting: LEGACY gap-inference — "
                "kernel BTF (/sys/kernel/btf/vmlinux) absent, so the "
                "sched_switch exact-CPU program cannot load. Exact-tier CPU is "
                "inferred from wait gaps (includes runqueue/throttle time); "
                "Off-CPU* is unavailable and trace cpu_ns carries UNKNOWN.\n");
    }

    /* Load BPF programs (runs verifier) */
    err = pg_wait_tracer_bpf__load(d->skel);
    if (err) {
        fprintf(stderr, "FATAL: BPF load failed: %s\n", strerror(-err));
        pg_wait_tracer_bpf__destroy(d->skel);
        d->skel = NULL;
        return -1;
    }

    /* Attach tracepoints (fork/exit — auto-attach) */
    d->skel->links.on_fork = bpf_program__attach(d->skel->progs.on_fork);
    if (!d->skel->links.on_fork) {
        fprintf(stderr, "FATAL: cannot attach on_fork tracepoint: %s\n",
                strerror(errno));
        goto fail;
    }

    /* S3: exact CPU accounting via sched_switch (docs/S3_SCHED_SWITCH_CPU.md).
     * Attached whenever measured CPU is on (BTF present). On the rare failure
     * (locked-down kernel) drop to gap-inference and say so — never silent. */
    if (d->cpu_accounting) {
        d->skel->links.on_sched_switch =
            bpf_program__attach(d->skel->progs.on_sched_switch);
        if (!d->skel->links.on_sched_switch) {
            d->cpu_accounting = 0;
            fprintf(stderr, "WARN: CPU accounting: could not attach "
                    "sched_switch (%s) — falling back to gap-inference; "
                    "Off-CPU* unavailable.\n", strerror(errno));
        } else if (d->verbose) {
            fprintf(stderr, "INFO: CPU accounting: MEASURED (sched_switch)\n");
        }
    }

    d->skel->links.on_exit = bpf_program__attach(d->skel->progs.on_exit);
    if (!d->skel->links.on_exit) {
        fprintf(stderr, "FATAL: cannot attach on_exit tracepoint: %s\n",
                strerror(errno));
        goto fail;
    }

    /* Attach query lifecycle probes (only in full trace mode) */
    if (!d->lightweight_mode && !d->skip_usdt && d->pg_binary_saved) {
        const char *bin = d->pg_binary_saved;

        /* Query-id capture uprobe. Must be active before USDT probes fire so
         * EXEC_START always has a correct query_id. Two variants:
         *   PG14+ : pgstat_report_query_id (arg = query_id), bypassing shmem.
         *   PG13  : no in-core query_id / no pgstat_report_query_id. Instead,
         *           with pg_stat_statements loaded (use_pg13_query_attr),
         *           uprobe standard_ExecutorStart(QueryDesc*) and walk
         *           queryDesc->plannedstmt->queryId. Feeds the SAME state_map
         *           slot, so the sampler + watchpoint pipelines are unchanged.
         *
         *           We probe standard_ExecutorStart, NOT the public
         *           ExecutorStart wrapper. With pg_stat_statements loaded,
         *           ExecutorStart_hook is set, so the ExecutorStart wrapper is
         *           a tiny trampoline that tail-jumps (jmp *hook) into the
         *           hook chain; an entry uprobe placed on that trampoline does
         *           not fire reliably (the tail-jump never returns through the
         *           wrapper body). standard_ExecutorStart is the real function
         *           and is always reached at the bottom of the hook chain
         *           (pgss's hook calls it), by which point pgss has already
         *           populated PlannedStmt.queryId. Same arg0 (QueryDesc*). */
        if (d->use_pg13_query_attr) {
            uint64_t es_va = pgwt_find_symbol_offset(bin, "standard_ExecutorStart");
            /* vaddr -> uprobe FILE offset via the ELF program headers. The
             * old `va - 0x400000` non-PIE heuristic attached the probe to a
             * dead byte on PIE builds — attach "succeeded", the probe never
             * fired, query attribution was silently zero (study defect 1). */
            uint64_t es_off = pgwt_vaddr_to_file_offset(bin, es_va);
            if (es_va && !es_off)
                fprintf(stderr, "WARN: cannot translate standard_ExecutorStart "
                        "VA 0x%lx to a file offset in %s (PG13 query "
                        "attribution disabled)\n", (unsigned long)es_va, bin);
            if (es_off) {
                LIBBPF_OPTS(bpf_uprobe_opts, uprobe_opts, .retprobe = false);
                d->skel->links.on_executor_start =
                    bpf_program__attach_uprobe_opts(d->skel->progs.on_executor_start,
                                                    -1, bin, es_off, &uprobe_opts);
                if (d->verbose)
                    fprintf(stderr, "INFO: standard_ExecutorStart at offset 0x%lx "
                            "(PG13 query attribution via pg_stat_statements)\n",
                            (unsigned long)es_off);
                if (!d->skel->links.on_executor_start)
                    fprintf(stderr, "WARN: could not attach standard_ExecutorStart "
                            "uprobe (PG13 query attribution disabled)\n");
            } else {
                fprintf(stderr, "WARN: symbol 'standard_ExecutorStart' not found "
                        "(PG13 query attribution disabled)\n");
            }
        } else {
            uint64_t qid_func_va = pgwt_find_symbol_offset(bin, "pgstat_report_query_id");
            uint64_t qid_func_off = pgwt_vaddr_to_file_offset(bin, qid_func_va);
            if (qid_func_va && !qid_func_off)
                fprintf(stderr, "WARN: cannot translate pgstat_report_query_id "
                        "VA 0x%lx to a file offset in %s (query attribution "
                        "disabled)\n", (unsigned long)qid_func_va, bin);
            if (qid_func_off) {
                LIBBPF_OPTS(bpf_uprobe_opts, uprobe_opts, .retprobe = false);
                d->skel->links.on_report_query_id =
                    bpf_program__attach_uprobe_opts(d->skel->progs.on_report_query_id,
                                                    -1, bin, qid_func_off, &uprobe_opts);
                if (d->verbose)
                    fprintf(stderr, "INFO: pgstat_report_query_id at offset 0x%lx\n",
                            (unsigned long)qid_func_off);
            }
        }

        /* Command-open gate uprobe (T2): pgstat_report_activity(state, ...)
         * maintains cmd_open in the state_map — the pg_stat_activity
         * state='active' window the sampler gates client on-CPU samples on,
         * and (while watchpoints are live) the CMD_START/CMD_END markers the
         * exact tier's we==0 classification uses. Attached in every
         * non-lightweight mode, like the query_id uprobe. Failure is loud:
         * without the gate, client CPU is not sampled — the CPU half of AAS
         * silently disappears for client backends otherwise. */
        {
            uint64_t act_va = pgwt_find_symbol_offset(bin, "pgstat_report_activity");
            uint64_t act_off = pgwt_vaddr_to_file_offset(bin, act_va);
            if (act_off && d->skel->rodata->bs_state_running != 0) {
                LIBBPF_OPTS(bpf_uprobe_opts, act_opts, .retprobe = false);
                d->skel->links.on_report_activity =
                    bpf_program__attach_uprobe_opts(d->skel->progs.on_report_activity,
                                                    -1, bin, act_off, &act_opts);
                if (d->verbose && d->skel->links.on_report_activity)
                    fprintf(stderr, "INFO: pgstat_report_activity at offset 0x%lx "
                            "(command-open gate)\n", (unsigned long)act_off);
            }
            d->cmd_gate_active = (d->skel->links.on_report_activity != NULL);
            if (!d->skel->links.on_report_activity)
                fprintf(stderr,
                        "WARN: command-open gate unavailable (%s) — on-CPU "
                        "samples for CLIENT backends will NOT be recorded; "
                        "sampled AAS under-counts CPU-bound client activity. "
                        "Background/parallel-worker CPU is unaffected.\n",
                        act_va == 0
                            ? "symbol 'pgstat_report_activity' not found"
                            : (act_off == 0
                                   ? "cannot translate VA to file offset"
                                   : (d->skel->rodata->bs_state_running == 0
                                          ? "unknown BackendState layout for this PG version"
                                          : "uprobe attach failed")));
        }

        /* USDT marker probes write into event_ringbuf, which only the FULL
         * tier consumes. In sampled mode they would be pure overhead with no
         * reader, so attach them only when watchpoints are in use. The
         * query_id uprobe above stays in every mode — the sampler joins
         * pid->query_id from the state_map it maintains. */
        if (pgwt_mode_uses_watchpoints(d)) {
            d->skel->links.on_exec_start =
                bpf_program__attach_usdt(d->skel->progs.on_exec_start,
                                         -1, bin,
                                         "postgresql", "query__execute__start", NULL);
            d->skel->links.on_exec_done =
                bpf_program__attach_usdt(d->skel->progs.on_exec_done,
                                         -1, bin,
                                         "postgresql", "query__execute__done", NULL);
            d->skel->links.on_plan_start =
                bpf_program__attach_usdt(d->skel->progs.on_plan_start,
                                         -1, bin,
                                         "postgresql", "query__plan__start", NULL);
            d->skel->links.on_plan_done =
                bpf_program__attach_usdt(d->skel->progs.on_plan_done,
                                         -1, bin,
                                         "postgresql", "query__plan__done", NULL);

            bool qid_attached = d->use_pg13_query_attr
                ? (d->skel->links.on_executor_start != NULL)
                : (d->skel->links.on_report_query_id != NULL);
            if (d->skel->links.on_exec_start && d->skel->links.on_exec_done
                && qid_attached)
                fprintf(stderr, "INFO: attached USDT + query_id uprobe\n");
            else
                fprintf(stderr, "WARN: could not attach query probes (lifecycle tracking disabled)\n");
        } else if (d->skel->links.on_report_query_id || d->skel->links.on_executor_start) {
            fprintf(stderr, "INFO: attached query_id uprobe (sampled mode)\n");
        }
    }

    /* Set up lifecycle ring buffer consumer */
    int rb_map_fd = bpf_map__fd(d->skel->maps.lifecycle_rb);
    d->rb = ring_buffer__new(rb_map_fd, handle_lifecycle_event, d, NULL);
    if (!d->rb) {
        fprintf(stderr, "FATAL: cannot create ring buffer: %s\n",
                strerror(errno));
        goto fail;
    }

    /* Set up event ring buffer consumer */
    d->event_accum = calloc(1, sizeof(*d->event_accum));
    if (!d->event_accum) {
        fprintf(stderr, "FATAL: cannot allocate event accumulator\n");
        goto fail;
    }
    pgwt_accum_init(d->event_accum);

    /* The event ringbuf consumer carries watchpoint transitions (FULL tier).
     * Lightweight mode accumulates in BPF; pure sampled mode arms no
     * watchpoints, so nothing is ever written there. TIERED mode starts
     * de-escalated (no watchpoints yet) but MUST have the ringbuf ready so an
     * escalation can begin consuming immediately — D5: skeleton + ringbuf are
     * loaded at daemon start, escalation only attaches watchpoints + starts
     * consuming. An idle ringbuf costs nothing. */
    bool needs_event_rb = !d->lightweight_mode
        && (pgwt_mode_uses_watchpoints(d) || d->mode == PGWT_MODE_TIERED);
    if (needs_event_rb) {
        int event_rb_map_fd = bpf_map__fd(d->skel->maps.event_ringbuf);
        d->event_rb = ring_buffer__new(event_rb_map_fd, pgwt_handle_trace_event, d, NULL);
        if (!d->event_rb) {
            fprintf(stderr, "FATAL: cannot create event ring buffer: %s\n",
                    strerror(errno));
            goto fail;
        }
    }

    /* Init trace file writer (opt-in via --trace-dir) */
    if (d->trace_dir) {
        d->event_writer = calloc(1, sizeof(*d->event_writer));
        if (!d->event_writer) {
            fprintf(stderr, "FATAL: cannot allocate event writer\n");
            goto fail;
        }
        int ret = d->trace_retention > 0 ? d->trace_retention : 24;
        if (pgwt_writer_init(d->event_writer, d->trace_dir,
                             d->pg_major_version, ret,
                             d->trace_group) != 0) {
            fprintf(stderr, "FATAL: cannot initialize trace writer\n");
            free(d->event_writer);
            d->event_writer = NULL;
            goto fail;
        }
        d->event_writer->verbose = d->verbose;
        /* T8: write real cpu_ns only when the capability probe confirmed
         * measured CPU; otherwise every TRANSITIONS record is stamped
         * PGWT_CPU_NS_UNKNOWN so readers fall back to gap-inference. */
        d->event_writer->cpu_measured = d->cpu_accounting;
        /* DUR-3: optional size cap on top of the hours-based retention. */
        if (d->trace_retention_gb > 0)
            d->event_writer->retention_bytes =
                (uint64_t)(d->trace_retention_gb * 1024.0 * 1024.0 * 1024.0);
        if (d->verbose)
            fprintf(stderr, "INFO: trace writer: %s (retention %dh%s)\n",
                    d->trace_dir, ret,
                    d->event_writer->retention_bytes ? ", size-capped" : "");

        /* Init summary writer (alongside event writer) */
        d->summary_writer = calloc(1, sizeof(*d->summary_writer));
        if (!d->summary_writer) {
            fprintf(stderr, "FATAL: cannot allocate summary writer\n");
            goto fail;
        }
        if (pgwt_summary_writer_init(d->summary_writer, d->trace_dir,
                                      ret, d->trace_group) != 0) {
            fprintf(stderr, "FATAL: cannot initialize summary writer\n");
            free(d->summary_writer);
            d->summary_writer = NULL;
            goto fail;
        }
        d->summary_writer->verbose = d->verbose;

        /* Init query text capture (requires st_activity_raw offset + MyBEEntry) */
        if (d->st_activity_offset > 0 && d->my_be_entry_addr != 0) {
            d->query_text_capture = calloc(1, sizeof(*d->query_text_capture));
            if (d->query_text_capture) {
                if (pgwt_qt_init(d->query_text_capture, d->trace_dir,
                                  d->my_be_entry_addr,
                                  d->st_activity_offset,
                                  d->event_writer->trace_gid) != 0) {
                    free(d->query_text_capture);
                    d->query_text_capture = NULL;
                } else {
                    d->query_text_capture->verbose = d->verbose;
                    if (d->verbose)
                        fprintf(stderr, "INFO: query text capture enabled\n");
                }
            }
        }

        /* Init backend metadata writer */
        d->backend_meta = calloc(1, sizeof(*d->backend_meta));
        if (d->backend_meta) {
            if (pgwt_bm_init(d->backend_meta, d->trace_dir) != 0) {
                free(d->backend_meta);
                d->backend_meta = NULL;
            } else if (d->verbose) {
                fprintf(stderr, "INFO: backend metadata writer enabled\n");
            }
        }
    }

    /* Scan existing backends (tracepoints are already attached,
     * so any new forks during scan will be caught) */
    int n = pgwt_scan_existing_backends(d);
    if (n == -2) {
        /* Runtime offset validation refused (garbage class byte). The old
         * code path degraded this to a WARN and kept running — the exact
         * silent-garbage failure CAP-2 exists to prevent. Abort. */
        goto fail;
    } else if (n < 0) {
        fprintf(stderr, "WARN: scan_existing_backends failed\n");
    } else if (d->verbose) {
        fprintf(stderr, "INFO: attached to %d existing backends\n", n);
    }

    /* CAP-2/3: if the scan resolved backends but none produced a NON-ZERO
     * class-valid reading, re-poll briefly and refuse to run on the
     * hardcoded-offset path (PG<17) if the offset still cannot be confirmed
     * — a wrong offset into zeroed memory reads zero forever, and blessing
     * it would trace garbage (or nothing) labeled as real. */
    if (pgwt_confirm_wait_offset(d) != 0)
        goto fail;

    /* Straddle-race recovery at startup. The one-shot scan above can miss a
     * pre-existing backend (its /proc read raced) or leave it in bootstrap
     * limbo (its wait_event_info resolve returned 0 transiently) — fatal for a
     * pure-CPU straddler whose bootstrap watchpoint never fires. Re-attempt a
     * few times over the first ~600ms: the transient that caused the miss is
     * gone by now, so the re-resolve succeeds and the backend is attached
     * BEFORE any measurement window opens (not one display interval later, as
     * the per-tick backstop in handle_timer would). Cheap no-op when the scan
     * already attached everything. */
    if (pgwt_mode_uses_watchpoints(d) && !getenv("PGWT_TEST_NO_RECOVERY")) {
        for (int i = 0; i < 3; i++) {
            pgwt_recover_unattached_backends(d);
            usleep(200000);   /* 200ms between passes */
        }
    }

    /* Create timer fd */
    d->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (d->timer_fd < 0) {
        perror("timerfd_create");
        goto fail;
    }
    struct itimerspec its = {
        .it_interval = { .tv_sec = d->interval },
        .it_value    = { .tv_sec = d->interval },
    };
    int timer_settime_rc = timerfd_settime(d->timer_fd, 0, &its, NULL);
    if (d->debug_dump_state && timer_settime_rc != 0)
        d->debug_timer_settime_rc = -errno;

    /* High-rate sampler timer (sampled/tiered tiers): fires at sample_rate_hz
     * independently of the per-second display interval. Armed ONE-SHOT with
     * per-tick phase jitter (SMP-5) and re-armed after every poll. */
    if (d->provider && d->provider->fidelity == PGWT_FIDELITY_SAMPLED) {
        d->sample_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (d->sample_timer_fd < 0) {
            perror("timerfd_create (sampler)");
            goto fail;
        }
        srand((unsigned)(time(NULL) ^ getpid()));
        arm_sample_timer_jittered(d);
    }

    /* Create signal fd */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    d->signal_fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (d->signal_fd < 0) {
        perror("signalfd");
        goto fail;
    }

    /* Create epoll */
    d->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (d->epoll_fd < 0) {
        perror("epoll_create1");
        goto fail;
    }

    struct epoll_event ev;

    /* Add lifecycle ring buffer fd */
    int rb_fd = ring_buffer__epoll_fd(d->rb);
    ev.events = EPOLLIN;
    ev.data.fd = rb_fd;
    epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD, rb_fd, &ev);

    /* Add event ring buffer fd (skip in lightweight mode) */
    int event_rb_fd = -1;
    if (d->event_rb) {
        event_rb_fd = ring_buffer__epoll_fd(d->event_rb);
        ev.events = EPOLLIN;
        ev.data.fd = event_rb_fd;
        epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD, event_rb_fd, &ev);
    }

    /* Add timer fd */
    ev.events = EPOLLIN;
    ev.data.fd = d->timer_fd;
    int timer_epoll_rc = epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD, d->timer_fd, &ev);
    if (d->debug_dump_state && timer_epoll_rc != 0)
        d->debug_timer_epoll_rc = -errno;

    /* Add sampler timer fd (sampled/tiered tiers) */
    if (d->sample_timer_fd >= 0) {
        ev.events = EPOLLIN;
        ev.data.fd = d->sample_timer_fd;
        epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD, d->sample_timer_fd, &ev);
    }

    /* Add signal fd */
    ev.events = EPOLLIN;
    ev.data.fd = d->signal_fd;
    epoll_ctl(d->epoll_fd, EPOLL_CTL_ADD, d->signal_fd, &ev);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    d->start_ts = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    d->running = true;

    /* Control socket at {trace_dir}/pgwt.sock (D4).
     * Failure is non-fatal (tracing must not depend on the control plane) —
     * EXCEPT a live-daemon conflict (ESC-10, rc == -2): another daemon already
     * owns this trace dir, so starting a second one would corrupt the trace and
     * steal the control plane. Refuse startup loudly. */
    if (d->trace_dir) {
        d->control = calloc(1, sizeof(*d->control));
        if (d->control) {
            int crc = pgwt_control_init(d->control, d, d->epoll_fd);
            if (crc == -2) {
                free(d->control);
                d->control = NULL;
                goto fail;   /* live daemon present — fatal */
            } else if (crc != 0) {
                free(d->control);
                d->control = NULL;
            } else if (d->verbose) {
                fprintf(stderr, "INFO: control socket: %s/pgwt.sock\n",
                        d->trace_dir);
            }
        }
    }

    /* Escalation engine (D5/A4). Enabled only in --mode tiered; creates the
     * bounded-window deadline timerfd on the daemon epoll. Failure is
     * non-fatal — tiered then runs sampled-only with no escalation path. */
    if (pgwt_escalation_init(d, d->escalation_budget_s) != 0) {
        fprintf(stderr, "WARN: escalation engine unavailable; "
                "tiered mode will run sampled-only\n");
    }

    /* Anomaly-trigger rules (A5): armed only when the escalation engine is
     * live (tiered mode with a working deadline timer). Rate-derived defaults
     * are then overridden by any --anomaly-* flags. */
    pgwt_anomaly_init(&d->anomaly, d->escalation.enabled, d->sample_rate_hz);
    if (d->anomaly.enabled) {
        if (d->anomaly_aas_factor > 0.0)
            d->anomaly.aas_factor = d->anomaly_aas_factor;
        if (d->anomaly_aas_ticks > 0)
            d->anomaly.aas_ticks = d->anomaly_aas_ticks;
        if (d->anomaly_lock_fraction >= 0.0)
            d->anomaly.lock_fraction = d->anomaly_lock_fraction;
        if (d->anomaly_lock_min_aas >= 0.0)
            d->anomaly.lock_min_aas = d->anomaly_lock_min_aas;
        if (d->anomaly_cooldown_s >= 0)
            d->anomaly.cooldown_ns =
                (uint64_t)d->anomaly_cooldown_s * 1000000000ULL;
        if (d->anomaly_window_s > 0)
            d->anomaly.escalation_s = d->anomaly_window_s;
        if (d->anomaly_cpu_min_aas > 0.0)
            d->anomaly.cpu_min_aas = d->anomaly_cpu_min_aas;
        if (d->anomaly_cpu_margin >= 0.0)
            d->anomaly.cpu_margin = d->anomaly_cpu_margin;
        if (d->anomaly_cpu_cusum_k >= 0.0)
            d->anomaly.cpu_cusum_k = d->anomaly_cpu_cusum_k;
        if (d->anomaly_cpu_cusum_h > 0.0)
            d->anomaly.cpu_cusum_h = d->anomaly_cpu_cusum_h;
        if (d->anomaly_cpu_cusum_cap > 0.0)
            d->anomaly.cpu_cusum_cap = d->anomaly_cpu_cusum_cap;
        if (d->anomaly_cpu_coverage_gap_s > 0.0)
            pgwt_anomaly_set_cpu_coverage_gap_s(
                &d->anomaly, d->anomaly_cpu_coverage_gap_s);
        if (d->anomaly_cpu_cusum_disabled)
            d->anomaly.cpu_cusum_enabled = false;
        if (d->verbose)
            fprintf(stderr,
                    "INFO: anomaly triggers armed: aas>%.1f*baseline for %d "
                    "ticks, lock_frac>%.2f for %d ticks, cooldown %llus, "
                    "window %ds, cpu_cusum=%s(min_aas=%.3f margin=%.3f "
                    "k=%.3f h=%.3f cap=%.3f coverage_gap=%.3fs/%d ticks)\n",
                    d->anomaly.aas_factor, d->anomaly.aas_ticks,
                    d->anomaly.lock_fraction, d->anomaly.lock_ticks,
                    (unsigned long long)(d->anomaly.cooldown_ns / 1000000000ULL),
                    d->anomaly.escalation_s,
                    (d->anomaly.cpu_cusum_enabled
                     && d->anomaly.aas_factor
                        < PGWT_ANOMALY_DISABLE_AAS_FACTOR)
                        ? "enabled" : "disabled",
                    d->anomaly.cpu_min_aas, d->anomaly.cpu_margin,
                    d->anomaly.cpu_cusum_k, d->anomaly.cpu_cusum_h,
                    d->anomaly.cpu_cusum_cap,
                    d->anomaly.cpu_coverage_gap_reset_s,
                    d->anomaly.cpu_coverage_gap_reset_ticks);
    }

    /* Arm the active capture provider. For the full tier this is a no-op
     * (watchpoints were armed during the backend scan); for the sampled tier
     * it allocates the sampler state. */
    if (d->provider && d->provider->start) {
        if (d->provider->start(d) != 0) {
            fprintf(stderr, "FATAL: capture provider '%s' failed to start\n",
                    d->provider->name);
            goto fail;
        }
    }
    if (d->verbose)
        fprintf(stderr, "INFO: capture provider: %s (%s fidelity)\n",
                d->provider->name,
                d->provider->fidelity == PGWT_FIDELITY_EXACT ? "exact" : "sampled");

    if (!d->quiet)
        pgwt_print_header(d);
    return 0;

fail:
    pgwt_daemon_cleanup(d);
    return -1;
}

int pgwt_daemon_run(struct pgwt_daemon *d)
{
    struct epoll_event events[8];
    int rb_fd = ring_buffer__epoll_fd(d->rb);
    int event_rb_fd = d->event_rb ? ring_buffer__epoll_fd(d->event_rb) : -1;
    bool event_ring_backlog = false;

    uint64_t deadline = 0;
    if (d->duration > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        deadline = (uint64_t)ts.tv_sec + d->duration;
    }

    /* BPF uses BPF_RB_NO_WAKEUP for event_ringbuf to avoid per-event
     * eventfd_signal overhead. We poll the ringbuf at 10ms intervals. */
    int poll_ms = d->event_rb ? 10 : -1;

    /* Baseline for the first completed-pass gap. Subsequent timestamps are
     * updated exactly once at the end of each main-loop pass. */
    if (d->debug_dump_state)
        d->debug_last_loop_ts_ns = pgwt_debug_monotonic_ns();

    while (d->running) {
        /* A budget yield means records remain. Re-enter epoll without sleeping
         * so ready timer/signal/control fds run before the next event batch;
         * if none are ready, immediately continue draining the backlog. */
        int timeout_ms = event_ring_backlog ? 0 : poll_ms;
        if (deadline > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            if ((uint64_t)ts.tv_sec >= deadline) break;
            int remaining = (deadline - ts.tv_sec) * 1000;
            if (timeout_ms < 0 || remaining < timeout_ms)
                timeout_ms = remaining;
        }

        uint64_t started_ns = pgwt_debug_block_begin(d);
        int n = epoll_wait(d->epoll_fd, events, 8, timeout_ms);
        pgwt_debug_block_end(d, "epoll_wait", 0, started_ns);
        if (n < 0) {
            if (errno == EINTR) {
                if (d->debug_dump_state) {
                    uint64_t now = pgwt_debug_monotonic_ns();
                    uint64_t gap = now - d->debug_last_loop_ts_ns;
                    if (gap > d->debug_max_loop_gap_ns)
                        d->debug_max_loop_gap_ns = gap;
                    d->debug_last_loop_ts_ns = now;
                    d->debug_loop_iterations++;
                }
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == d->timer_fd) {
                started_ns = pgwt_debug_block_begin(d);
                handle_timer(d);
                pgwt_debug_block_end(d, "timer_handler", 0, started_ns);
            } else if (d->sample_timer_fd >= 0
                       && events[i].data.fd == d->sample_timer_fd) {
                started_ns = pgwt_debug_block_begin(d);
                handle_sample_timer(d);
                pgwt_debug_block_end(d, "sample_timer_handler", 0, started_ns);
            } else if (events[i].data.fd == rb_fd) {
                started_ns = pgwt_debug_block_begin(d);
                ring_buffer__consume(d->rb);
                pgwt_debug_block_end(d, "lifecycle_ring_consume", 0, started_ns);
            } else if (events[i].data.fd == event_rb_fd) {
                /* The common bounded drain below handles this readiness. Doing
                 * it here as well would spend two callback budgets per pass. */
            } else if (events[i].data.fd == d->signal_fd) {
                started_ns = pgwt_debug_block_begin(d);
                handle_signal(d);
                pgwt_debug_block_end(d, "signal_handler", 0, started_ns);
            } else if (pgwt_escalation_is_timer_fd(d, events[i].data.fd)) {
                started_ns = pgwt_debug_block_begin(d);
                pgwt_escalation_on_timer(d);
                pgwt_debug_block_end(d, "escalation_timer_handler", 0,
                                     started_ns);
            } else if (d->control) {
                started_ns = pgwt_debug_block_begin(d);
                pgwt_control_handle_fd(d->control, events[i].data.fd);
                pgwt_debug_block_end(d, "control_handler", 0, started_ns);
            }
        }

        /* Drain event ringbuf on every iteration (poll-based with NO_WAKEUP) */
        if (d->event_rb) {
            event_ring_backlog = consume_event_ring(
                d, "event_ring_drain", true);
        }

        if (d->debug_dump_state) {
            uint64_t now = pgwt_debug_monotonic_ns();
            uint64_t gap = now - d->debug_last_loop_ts_ns;
            if (gap > d->debug_max_loop_gap_ns)
                d->debug_max_loop_gap_ns = gap;
            d->debug_last_loop_ts_ns = now;
            d->debug_loop_iterations++;
        }
    }

    /* Drain remaining events before cleanup */
    if (d->event_rb) {
        consume_event_ring(d, "event_ring_final_drain", false);
    }
    uint64_t started_ns = pgwt_debug_block_begin(d);
    ring_buffer__consume(d->rb);
    pgwt_debug_block_end(d, "lifecycle_ring_final_drain", 0, started_ns);

    if (d->exit_reason != PGWT_EXIT_PG_DEAD)
        fprintf(stderr, "\npg_wait_tracer: shutting down\n");
    return 0;
}

/* PGWT_DEBUG_DUMP_STATE (DEBUG ONLY): dump the final BPF state_map to stderr
 * at shutdown — the self-diagnosing payload for the reopened straddle
 * live-CPU flake (docs/ROADMAP_AND_STATUS.md "REOPENED 2026-08-04 — a FOURTH
 * straddle live-CPU mode exists"). Three shipped fixes each closed a distinct
 * mode (#52 scan recovery, #56 fire-time on_cpu_ts open, #63 stale-state
 * sweep) yet a `live CPU* = 0ms, trace correct` run still occurs, and every
 * CI hit so far cost an inference cycle because the hog's end-state died with
 * the daemon. This prints each entry's RAW fields exactly as the last live
 * tick saw them — decoded alongside, fabricating nothing (no clamping, no
 * derived verdicts) — plus one STATEDUMP-META line with the self-metrics
 * (control.c names) that discriminate between the known modes. Capped at
 * PGWT_STATEDUMP_MAX entries: the flaky phase tracks a handful of backends,
 * and a capped dump on a big map says so loudly instead of flooding.
 * With the env unset, the cached gate skips the clock reads and emits no
 * diagnostic output. */
#define PGWT_STATEDUMP_MAX 64

static void pgwt_debug_dump_state_map(struct pgwt_daemon *d)
{
    if (!d->debug_dump_state || !d->skel)
        return;
    int state_fd = bpf_map__fd(d->skel->maps.state_map);
    if (state_fd < 0)
        return;   /* init-failure path: skeleton opened but never loaded */

    /* Same clock base as bpf_ktime / attach_ts (see preseed_state_map), so
     * the printed ages are directly comparable to the entry timestamps. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

    int backends_tracked = 0;
    for (int i = 0; i < d->backends.count; i++)
        if (d->backends.entries[i].is_alive)
            backends_tracked++;

    double last_loop_age_ms = d->debug_last_loop_ts_ns
        ? ((double)now - (double)d->debug_last_loop_ts_ns) / 1e6 : -1.0;

    /* META first so a truncated/partial log still carries the counters. */
    fprintf(stderr,
            "STATEDUMP-META: cpu_accounting=%d backends_tracked=%d "
            "loop_iterations=%llu timer_ticks=%llu completed_ticks=%d "
            "timer_expirations=%llu max_timer_expirations=%llu "
            "max_loop_gap_ms=%.3f last_loop_age_ms=%.3f "
            "event_callbacks_total=%llu callback_ms_total=%.3f "
            "max_callback_ms=%.3f event_drain_yields=%llu "
            "test_delays_left=%u "
            "timer_settime_rc=%d timer_epoll_rc=%d "
            "state_reseeds_total=%llu invalid_wait_reads_total=%llu "
            "wp_attach_failures_total=%llu state_map_full_total=%llu "
            "cpu_ns_total=%llu offcpu_ns_total=%llu "
            "cpu_clamped_ns_total=%llu wait_gap_cpu_ns_total=%llu\n",
            d->cpu_accounting ? 1 : 0, backends_tracked,
            (unsigned long long)d->debug_loop_iterations,
            (unsigned long long)d->debug_timer_entries,
            d->tick,
            (unsigned long long)d->debug_timer_expirations,
            (unsigned long long)d->debug_max_timer_expirations,
            (double)d->debug_max_loop_gap_ns / 1e6,
            last_loop_age_ms,
            (unsigned long long)d->debug_event_callbacks_total,
            (double)d->debug_event_callback_ns_total / 1e6,
            (double)d->debug_event_callback_max_ns / 1e6,
            (unsigned long long)d->debug_event_drain_yields,
            d->test_event_callback_delays_left,
            d->debug_timer_settime_rc,
            d->debug_timer_epoll_rc,
            (unsigned long long)d->counters.state_reseeds_total,
            (unsigned long long)d->counters.invalid_wait_reads_total,
            (unsigned long long)d->counters.wp_attach_failures_total,
            (unsigned long long)d->counters.state_map_full_total,
            (unsigned long long)d->counters.cpu_ns_total,
            (unsigned long long)d->counters.offcpu_ns_total,
            (unsigned long long)d->counters.cpu_clamped_ns_total,
            (unsigned long long)d->counters.wait_gap_cpu_ns_total);

    uint32_t key = 0, next;
    struct pgwt_pid_state st;
    int n = 0;
    while (bpf_map_get_next_key(state_fd, &key, &next) == 0) {
        if (bpf_map_lookup_elem(state_fd, &next, &st) == 0) {
            if (++n > PGWT_STATEDUMP_MAX) {
                fprintf(stderr, "STATEDUMP: ... truncated at %d entries "
                        "(map holds more)\n", PGWT_STATEDUMP_MAX);
                break;
            }
            char ev_name[80];
            pgwt_event_full_name(st.last_event, ev_name, sizeof(ev_name));
            /* on_cpu_ts: 0 = no open on-CPU stretch (the straddle-flake
             * signature when paired with a stale wait label); nonzero is
             * printed raw with its age. Ages are signed on purpose — a
             * negative age would itself be a finding (clock skew), never
             * clamped away. */
            char oncpu[64];
            if (st.on_cpu_ts)
                snprintf(oncpu, sizeof(oncpu), "%llu(age_ms=%.1f)",
                         (unsigned long long)st.on_cpu_ts,
                         ((double)now - (double)st.on_cpu_ts) / 1e6);
            else
                snprintf(oncpu, sizeof(oncpu), "0");
            fprintf(stderr,
                    "STATEDUMP: pid=%u wp_live=%u cmd_open=%u "
                    "last_event=0x%08x(%s) last_ts=%llu(age_ms=%.1f) "
                    "on_cpu_ts=%s cpu_ns_total=%llu last_cpu_ns=%llu "
                    "last_query_id=0x%llx\n",
                    next, st.wp_live, st.cmd_open, st.last_event, ev_name,
                    (unsigned long long)st.last_ts,
                    ((double)now - (double)st.last_ts) / 1e6,
                    oncpu,
                    (unsigned long long)st.cpu_ns_total,
                    (unsigned long long)st.last_cpu_ns,
                    (unsigned long long)st.last_query_id);
        }
        key = next;
    }
}

void pgwt_daemon_cleanup(struct pgwt_daemon *d)
{
    /* DEBUG (PGWT_DEBUG_DUMP_STATE): capture the state_map's end-state BEFORE
     * anything below detaches watchpoints or flushes entries — the dump must
     * show exactly what the last live tick saw. No-op unless the env is set. */
    pgwt_debug_dump_state_map(d);

    /* Close any open escalation window (detaches watchpoints, writes the END
     * marker) before the event writer is torn down. */
    pgwt_escalation_cleanup(d);

    /* T8 symptom #2: flush open exact intervals (incl. the on-CPU stretch of a
     * command straddling capture end) with their measured cpu_ns BEFORE the
     * provider detaches watchpoints and the writer closes. In full mode this is
     * the ONLY flush path (no de-escalation); in tiered mode a just-closed
     * window already flushed+detached, so this is a no-op. */
    pgwt_flush_open_intervals(d);

    /* Stop the active capture provider (detach/free its state). */
    if (d->provider && d->provider->stop)
        d->provider->stop(d);

    if (d->control) {
        pgwt_control_cleanup(d->control);
        free(d->control);
        d->control = NULL;
    }
    if (d->backend_meta) {
        pgwt_bm_close(d->backend_meta);
        free(d->backend_meta);
        d->backend_meta = NULL;
    }
    if (d->query_text_capture) {
        if (d->query_text_capture->verbose)
            fprintf(stderr, "INFO: query text capture: %d unique queries captured\n",
                    d->query_text_capture->num_seen);
        pgwt_qt_close(d->query_text_capture);
        free(d->query_text_capture);
        d->query_text_capture = NULL;
    }
    if (d->summary_writer) {
        pgwt_summary_close(d->summary_writer);
        pgwt_summary_destroy(d->summary_writer);
        free(d->summary_writer);
        d->summary_writer = NULL;
    }
    if (d->event_writer) {
        pgwt_writer_close(d->event_writer);
        pgwt_writer_destroy(d->event_writer);
        free(d->event_writer);
        d->event_writer = NULL;
    }

    pgwt_ring_free(&d->ring);
    pgwt_close_all_backends(&d->backends);

    if (d->event_rb) {
        ring_buffer__free(d->event_rb);
        d->event_rb = NULL;
    }
    if (d->event_accum) {
        free(d->event_accum);
        d->event_accum = NULL;
    }
    if (d->rb) {
        ring_buffer__free(d->rb);
        d->rb = NULL;
    }
    if (d->skel) {
        pg_wait_tracer_bpf__destroy(d->skel);
        d->skel = NULL;
    }
    if (d->epoll_fd >= 0) { close(d->epoll_fd); d->epoll_fd = -1; }
    if (d->timer_fd >= 0) { close(d->timer_fd); d->timer_fd = -1; }
    if (d->sample_timer_fd >= 0) { close(d->sample_timer_fd); d->sample_timer_fd = -1; }
    if (d->signal_fd >= 0) { close(d->signal_fd); d->signal_fd = -1; }
}
