/* event_stream.c — BPF ringbuf event consumer + in-memory accumulation
 *
 * Processes raw trace events from event_ringbuf, accumulates into
 * d->event_accum. At timer tick, event_accum is copied to d->accum
 * for display (pgwt_accum_copy_used). */
#include "event_stream.h"
#include "daemon.h"
#include "event_writer.h"
#include "summary_writer.h"
#include "query_text.h"
#include "map_reader.h"
#include "wait_event.h"
#include "sampler.h"   /* pgwt_backend_type_flag (T2 category mapping) */

#include <string.h>
#include <time.h>
#include <errno.h>

/* duration_to_bucket() moved to map_reader.c as pgwt_duration_to_bucket() */

void pgwt_counters_add_cpu(struct pgwt_daemon *d, uint32_t we,
                            uint64_t dur_ns, uint64_t cpu_ns)
{
    /* Only measured records carry a real cpu_ns; markers and legacy/UNKNOWN
     * records contribute nothing (T8 §5.6). */
    if (cpu_ns == PGWT_CPU_NS_UNKNOWN || PGWT_IS_MARKER(we))
        return;
    if (we == 0) {
        /* On-CPU gap. A stale EXACTLY-0 delta (two boundary reads inside one
         * scheduler tick, so se.sum_exec_runtime did not advance) is a missing
         * measurement for a RUNNING gap, not "0 CPU" — fall back to gap-
         * inference (full gap as CPU), mirroring compute so the counter never
         * under-reports a finely-fragmented on-CPU command. A nonzero delta
         * splits: clamp to the wall gap (clock skew shows up as cpu_ns > dur),
         * the remainder is off-CPU/runqueue-unaccounted. */
        if (cpu_ns == 0) {
            d->counters.cpu_ns_total += dur_ns;
        } else {
            uint64_t cpu = cpu_ns;
            if (cpu > dur_ns) {
                d->counters.cpu_clamped_ns_total += cpu - dur_ns;
                cpu = dur_ns;
            }
            d->counters.cpu_ns_total += cpu;
            d->counters.offcpu_ns_total += dur_ns - cpu;
        }
    } else {
        /* Wait-labeled gap: measured CPU here should be ≈0 (a sleeping task
         * burns none) — the trace's own CPU-accounting self-check. */
        d->counters.wait_gap_cpu_ns_total += cpu_ns;
    }
}

int pgwt_handle_trace_event(void *ctx, void *data, size_t data_sz)
{
    struct pgwt_daemon *d = ctx;
    struct pgwt_trace_event *evt = data;
    struct pgwt_accumulator *acc = d->event_accum;
    uint64_t callback_started_ns = pgwt_debug_block_begin(d);
    uint64_t stage_started_ns;

    (void)data_sz;

    d->counters.events_total++;
    if (d->debug_dump_state)
        d->debug_event_callbacks_total++;

    if (d->test_event_callback_delay_us
        && d->test_event_callback_delays_left > 0) {
        d->test_event_callback_delays_left--;
        struct timespec requested = {
            .tv_sec = d->test_event_callback_delay_us / 1000000U,
            .tv_nsec = (long)(d->test_event_callback_delay_us % 1000000U)
                     * 1000L,
        };
        stage_started_ns = pgwt_debug_block_begin(d);
        while (nanosleep(&requested, &requested) != 0 && errno == EINTR)
            ;
        pgwt_debug_block_end(d, "event_callback_test_delay", evt->pid,
                             stage_started_ns);
    }

    /* Write to trace file if recording is enabled (including markers) */
    if (d->event_writer) {
        stage_started_ns = pgwt_debug_block_begin(d);
        pgwt_writer_push_event(d->event_writer, evt);
        pgwt_debug_block_end(d, "event_callback_trace_writer", evt->pid,
                             stage_started_ns);
    }

    /* Write to summary file if recording is enabled. Markers pass through
     * here too, but the summary writer's accum_event filters them (FID-4):
     * they must land in the trace (variants / lifecycle / escalation
     * coverage need them) while never entering wait accounting. */
    if (d->summary_writer) {
        stage_started_ns = pgwt_debug_block_begin(d);
        pgwt_summary_push_event(d->summary_writer, evt);
        pgwt_debug_block_end(d, "event_callback_summary_writer", evt->pid,
                             stage_started_ns);
    }

    uint32_t we = evt->old_event;
    uint64_t dur = evt->duration_ns;

    /* Query text is now captured by BPF uprobe (debug_query_string)
     * and delivered via lifecycle_rb, not from /proc/pid/mem here. */

    /* Skip accumulation for marker events (duration=0, not real waits) */
    if (PGWT_IS_MARKER(we))
        goto done;

    /* T8: fold the measured CPU of this closed interval into the lifetime
     * counters (observability + the wait-gap self-check). Display-time CPU
     * accounting is unchanged — this only reads evt->cpu_ns. */
    stage_started_ns = pgwt_debug_block_begin(d);
    pgwt_counters_add_cpu(d, we, dur, evt->cpu_ns);
    pgwt_debug_block_end(d, "event_callback_cpu_counters", evt->pid,
                         stage_started_ns);

    /* Per-PID accumulation */
    stage_started_ns = pgwt_debug_block_begin(d);
    struct pgwt_pid_accum *pa = pgwt_get_or_create_pid(acc, evt->pid);

    /* T2 live classification (decomposed AAS, docs/AAS_SEMANTICS_DECISION.md).
     * Resolve the pid's category ONCE against the registry and cache it in
     * the accumulator (the hot path must not linear-scan 1024 entries per
     * event). io_worker time never enters DB Time/AAS; a client backend's
     * we==0 interval outside a command (BPF stamps the gate in flags) is
     * post/between-command time — idle, not CPU. Both stay VISIBLE in the
     * per-event stats; only the load accounting differs. */
    int io_worker = 0;
    int noncmd_cpu = 0;
    if (pa) {
        if (pa->cat_flag_plus1 == 0) {
            struct pgwt_backend *be = pgwt_find_backend(&d->backends, evt->pid);
            uint32_t f = (be && be->meta_parsed)
                       ? pgwt_backend_type_flag(be->meta.backend_type) : 0;
            pa->cat_flag_plus1 = f + 1;
        }
        uint32_t cat = pa->cat_flag_plus1 - 1;
        io_worker = (cat == PGWT_EVENT_FLAG_IO_WORKER);
        noncmd_cpu = (we == 0 && cat == 0 && d->cmd_gate_active
                      && !(evt->flags & PGWT_EVENT_FLAG_CMD_OPEN));
    }

    if (pa) {
        struct pgwt_event_stats *es = pgwt_get_or_create_event(pa, we);
        if (es) {
            es->count++;
            es->total_ns += dur;
            if (dur < es->min_ns) es->min_ns = dur;
            if (dur > es->max_ns) es->max_ns = dur;
            uint32_t bucket = pgwt_duration_to_bucket(dur);
            if (bucket < HISTOGRAM_BUCKETS)
                es->histogram[bucket]++;
        }

        if (!io_worker && !noncmd_cpu) {
            if (we == 0) {
                pa->cpu_time_ns += dur;
            } else if (!pgwt_is_idle_event(we)) {
                pa->wait_time_ns += dur;
            }
            if (we == 0 || !pgwt_is_idle_event(we))
                pa->db_time_ns += dur;
        }
    }
    pgwt_debug_block_end(d, "event_callback_pid_accumulator", evt->pid,
                         stage_started_ns);

    /* System-wide accumulation */
    stage_started_ns = pgwt_debug_block_begin(d);
    struct pgwt_event_stats *se = pgwt_get_or_create_system_event(acc, we);
    if (se) {
        se->count++;
        se->total_ns += dur;
        if (dur < se->min_ns) se->min_ns = dur;
        if (dur > se->max_ns) se->max_ns = dur;
        uint32_t bucket = pgwt_duration_to_bucket(dur);
        if (bucket < HISTOGRAM_BUCKETS)
            se->histogram[bucket]++;
    }
    pgwt_debug_block_end(d, "event_callback_system_accumulator", evt->pid,
                         stage_started_ns);

    /* Time model by class. io_worker time is excluded from DB Time; a
     * non-command CPU interval lands in the idle (Activity) bucket. */
    stage_started_ns = pgwt_debug_block_begin(d);
    if (!io_worker)
        pgwt_update_time_model(&acc->tm,
                               noncmd_cpu ? WEI(PG_WAIT_ACTIVITY, 0) : we,
                               dur);
    pgwt_debug_block_end(d, "event_callback_time_model", evt->pid,
                         stage_started_ns);

    /* Query events */
    stage_started_ns = pgwt_debug_block_begin(d);
    if (evt->query_id != 0 && !io_worker) {
        struct pgwt_query_event_stats *qe =
            pgwt_get_or_create_query_event(acc, evt->query_id, we);
        if (qe) {
            qe->count++;
            qe->total_ns += dur;
            if (dur < qe->min_ns) qe->min_ns = dur;
            if (dur > qe->max_ns) qe->max_ns = dur;
        }
    }

    pgwt_debug_block_end(d, "event_callback_query_accumulator", evt->pid,
                         stage_started_ns);

done:
    if (callback_started_ns) {
        uint64_t callback_ended_ns = pgwt_debug_monotonic_ns();
        uint64_t callback_ns = callback_ended_ns >= callback_started_ns
                             ? callback_ended_ns - callback_started_ns : 0;
        d->debug_event_callback_ns_total += callback_ns;
        if (callback_ns > d->debug_event_callback_max_ns)
            d->debug_event_callback_max_ns = callback_ns;
        pgwt_debug_block_report("event_callback_total", evt->pid,
                                callback_started_ns);
    }
    d->event_drain_callbacks_current++;
    if (d->event_drain_callback_limit
        && d->event_drain_callbacks_current >= d->event_drain_callback_limit)
        return -EAGAIN;  /* record is fully processed; ask libbpf to yield */
    return 0;
}

void pgwt_accum_copy_used(struct pgwt_accumulator *dst,
                           const struct pgwt_accumulator *src)
{
    /* Time model */
    dst->tm = src->tm;

    /* Per-PID data: copy only populated entries */
    dst->num_pids = src->num_pids;
    for (int i = 0; i < src->num_pids; i++) {
        const struct pgwt_pid_accum *sp = &src->pids[i];
        struct pgwt_pid_accum *dp = &dst->pids[i];

        dp->pid = sp->pid;
        dp->active = sp->active;
        dp->num_events = sp->num_events;
        dp->db_time_ns = sp->db_time_ns;
        dp->cpu_time_ns = sp->cpu_time_ns;
        dp->wait_time_ns = sp->wait_time_ns;
        dp->current_event = 0;      /* filled by pgwt_read_state_map */
        dp->current_wait_ns = 0;
        memcpy(dp->events, sp->events,
               sp->num_events * sizeof(struct pgwt_event_stats));
    }

    /* System-wide events */
    dst->num_system_events = src->num_system_events;
    memcpy(dst->system_events, src->system_events,
           src->num_system_events * sizeof(struct pgwt_event_stats));

    /* Query events */
    dst->num_query_events = src->num_query_events;
    memcpy(dst->query_events, src->query_events,
           src->num_query_events * sizeof(struct pgwt_query_event_stats));
}
