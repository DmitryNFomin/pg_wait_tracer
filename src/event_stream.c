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

    /* Markers carry no wait duration. The CMD_START/CMD_END pair (the
     * on_report_activity uprobe's gate flips, in this pid's stream order)
     * drives the live command gate (#98) — the same sweep the server runs
     * in pgwt_tag_events — and is otherwise skipped like every marker. */
    if (PGWT_IS_MARKER(we)) {
        if (we == PGWT_MARKER_CMD_START || we == PGWT_MARKER_CMD_END) {
            struct pgwt_pid_accum *mpa = pgwt_get_or_create_pid(acc, evt->pid);
            if (mpa) {
                pgwt_live_cmd_gate_marker(&mpa->cmd_gate, we,
                                          evt->timestamp_ns);
                d->counters.live_cmd_markers_total++;
            }
        }
        goto done;
    }

    /* T8: fold the measured CPU of this closed interval into the lifetime
     * counters (observability + the wait-gap self-check). Display-time CPU
     * accounting is unchanged — this only reads evt->cpu_ns. */
    stage_started_ns = pgwt_debug_block_begin(d);
    pgwt_counters_add_cpu(d, we, dur, evt->cpu_ns);
    pgwt_debug_block_end(d, "event_callback_cpu_counters", evt->pid,
                         stage_started_ns);

    /* Accumulate (per-pid, system, time model, query) through the ONE
     * live-interval function shared with the open-interval scan
     * (map_reader.c pgwt_accum_add_interval, issue #97): the closed record
     * and the open stretch it later closes must classify identically, or
     * the multi-window delta of the two ring snapshots shows a quantity no
     * snapshot ever had. T2 classification (decomposed AAS,
     * docs/AAS_SEMANTICS_DECISION.md): the pid's category is resolved
     * against the registry and cached in the accumulator (the hot path must
     * not linear-scan 1024 entries per event); io_worker time never enters
     * DB Time/AAS; a client backend's we==0 interval outside a command is
     * post/between-command time — idle, not CPU — and is filed under
     * PGWT_WEI_NONCMD_CPU in EVERY accumulator (it used to sit in the CPU*
     * row while its time went to Activity, so the CPU* row could exceed DB
     * Time). "Outside a command" is the marker majority rule over the
     * record's own [t1 - dur, t1) (#98), consumed from the pid's gate —
     * NOT the gate value BPF stamped at emission (PGWT_EVENT_FLAG_CMD_OPEN):
     * PostgreSQL reports STATE_IDLE before the post-command ClientRead
     * begins, so at emission a waitless statement's whole on-CPU run looked
     * non-command and ~83% of pgbench's CPU vanished from live DB Time. The
     * emission flag remains only the fallback for a full accumulator. */
    stage_started_ns = pgwt_debug_block_begin(d);
    struct pgwt_pid_accum *pa = pgwt_get_or_create_pid(acc, evt->pid);
    uint32_t cat_flag = pgwt_live_pid_cat_flag(d, pa, evt->pid);
    uint64_t t1 = evt->timestamp_ns;
    uint64_t t0 = t1 >= dur ? t1 - dur : 0;
    bool in_cmd = false;
    bool by_markers = pgwt_live_cmd_gate_classify(
        pa ? &pa->cmd_gate : NULL, true, t0, t1,
        (evt->flags & PGWT_EVENT_FLAG_CMD_OPEN) != 0, &in_cmd);
    if (!by_markers && we == 0 && cat_flag == 0 && d->cmd_gate_active) {
        /* Not decided by markers: say so (metrics), never silently. */
        if (pa)
            d->counters.live_cpu_unmarked_ns_total += dur;
        else
            d->counters.live_cpu_gate_fallback_total++;
    }
    struct pgwt_live_interval iv = {
        .pid             = evt->pid,
        .we              = we,
        .wall_ns         = dur,
        .cpu_ns          = dur,      /* live display accounts a closed on-CPU
                                      * segment at wall (measured cpu_ns is
                                      * folded into the lifetime counters
                                      * above); see ROADMAP "Multi-window %DB" */
        .query_id        = evt->query_id,
        .cat_flag        = cat_flag,
        .cmd_gate_active = d->cmd_gate_active,
        .cmd_open        = in_cmd,
        .closed          = true,
        .pa              = pa,
    };
    pgwt_accum_add_interval(acc, &iv);
    pgwt_debug_block_end(d, "event_callback_accumulate", evt->pid,
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
        dp->cat_flag_plus1 = sp->cat_flag_plus1;   /* the open scan reuses it */
        dp->cat_scan_tick = sp->cat_scan_tick;
        dp->cmd_gate = sp->cmd_gate;               /* #98: the open scan peeks it */
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
